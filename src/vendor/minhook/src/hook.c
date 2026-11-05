/*
 *  MinHook - The Minimalistic API Hooking Library for x64/x86
 *  Copyright (C) 2009-2017 Tsuda Kageyu.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
 *  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <limits.h>

#include "../include/MinHook.h"
#include "buffer.h"
#include "trampoline.h"

#ifndef ARRAYSIZE
    #define ARRAYSIZE(A) (sizeof(A)/sizeof((A)[0]))
#endif

// Initial capacity of the HOOK_ENTRY buffer.
#define INITIAL_HOOK_CAPACITY   32

// Initial capacity of the thread IDs buffer.
#define INITIAL_THREAD_CAPACITY 128

// Special hook position values.
#define INVALID_HOOK_POS UINT_MAX
#define ALL_HOOKS_POS    UINT_MAX

// Patch action argument defines.
#define ACTION_DISABLE      0
#define ACTION_ENABLE       1
#define ACTION_APPLY_QUEUED 2

// Thread access rights for suspending/resuming threads.
#define THREAD_ACCESS \
    (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SET_CONTEXT | SYNCHRONIZE)

// Hook information.
typedef struct _HOOK_ENTRY
{
    LPVOID pTarget;             // Address of the target function.
    LPVOID pDetour;             // Address of the detour or relay function.
    LPVOID pTrampoline;         // Address of the trampoline function.
    UINT8  backup[8];           // Original prologue of the target function.
    // ExCamera: the whole relocated prologue, so re-enabling a retained
    // trampoline never silently skips code that changed while it was stopped.
    UINT8  entryBackup[32];
    UINT8  entrySize;

    UINT8  patchAbove  : 1;     // Uses the hot patch area.
    UINT8  isEnabled   : 1;     // Enabled.
    UINT8  queueEnable : 1;     // Queued for enabling/disabling when != isEnabled.
    UINT8  published   : 1;     // A caller, entry, or thread may retain this trampoline.

    UINT   nIP : 4;             // Count of the instruction boundaries.
    UINT8  oldIPs[8];           // Instruction boundaries of the target function.
    UINT8  newIPs[8];           // Instruction boundaries of the trampoline function.
} HOOK_ENTRY, *PHOOK_ENTRY;

// Keep the exact handles and contexts until the matching resume. Reopening a
// thread ID can fail or refer to a reused ID after the snapshot was taken.
typedef struct _FROZEN_THREAD
{
    DWORD   id;
    HANDLE  handle;
    BOOL    suspended;
    BOOL    contextPending;
    DWORD_PTR pendingIP;
    CONTEXT original;
    CONTEXT current;
} FROZEN_THREAD, *PFROZEN_THREAD;

typedef struct _FROZEN_THREADS
{
    PFROZEN_THREAD pItems;  // Data heap
    UINT    capacity;       // Size of allocated data heap, items
    UINT    size;           // Actual number of data items
} FROZEN_THREADS, *PFROZEN_THREADS;

typedef struct _PATCH_PAGE
{
    LPVOID address;
    DWORD  protection;
    BOOL   writable;
} PATCH_PAGE, *PPATCH_PAGE;

typedef struct _PATCH_ENTRY
{
    PHOOK_ENTRY hook;
    LPBYTE      address;
    SIZE_T      size;
    BOOL        enable;
} PATCH_ENTRY, *PPATCH_ENTRY;

//-------------------------------------------------------------------------
// Global Variables:
//-------------------------------------------------------------------------

// Spin lock flag for EnterSpinLock()/LeaveSpinLock().
volatile LONG g_isLocked = FALSE;

// Private heap handle. If not NULL, this library is initialized.
HANDLE g_hHeap = NULL;

// Hook entries.
struct
{
    PHOOK_ENTRY pItems;     // Data heap
    UINT        capacity;   // Size of allocated data heap, items
    UINT        size;       // Actual number of data items
} g_hooks;

// A failed OS cleanup must not lose the information needed to finish it. New
// changes and teardown first retry the exact outstanding contexts, resumes,
// and protections.
FROZEN_THREADS g_pendingThreads;
PPATCH_PAGE g_pendingPages = NULL;
UINT g_pendingPageCount = 0;
PPATCH_ENTRY g_pendingFlush = NULL;
UINT g_pendingFlushCount = 0;
DWORD g_pageSize = 0;

//-------------------------------------------------------------------------
// Returns INVALID_HOOK_POS if not found.
static UINT FindHookEntry(LPVOID pTarget)
{
    UINT i;
    for (i = 0; i < g_hooks.size; ++i)
    {
        if ((ULONG_PTR)pTarget == (ULONG_PTR)g_hooks.pItems[i].pTarget)
            return i;
    }

    return INVALID_HOOK_POS;
}

//-------------------------------------------------------------------------
static PHOOK_ENTRY AddHookEntry()
{
    if (g_hooks.pItems == NULL)
    {
        g_hooks.capacity = INITIAL_HOOK_CAPACITY;
        g_hooks.pItems = (PHOOK_ENTRY)HeapAlloc(
            g_hHeap, 0, g_hooks.capacity * sizeof(HOOK_ENTRY));
        if (g_hooks.pItems == NULL)
            return NULL;
    }
    else if (g_hooks.size >= g_hooks.capacity)
    {
        PHOOK_ENTRY p = (PHOOK_ENTRY)HeapReAlloc(
            g_hHeap, 0, g_hooks.pItems, (g_hooks.capacity * 2) * sizeof(HOOK_ENTRY));
        if (p == NULL)
            return NULL;

        g_hooks.capacity *= 2;
        g_hooks.pItems = p;
    }

    return &g_hooks.pItems[g_hooks.size++];
}

//-------------------------------------------------------------------------
static VOID DeleteHookEntry(UINT pos)
{
    if (pos < g_hooks.size - 1)
        g_hooks.pItems[pos] = g_hooks.pItems[g_hooks.size - 1];

    g_hooks.size--;

    if (g_hooks.capacity / 2 >= INITIAL_HOOK_CAPACITY && g_hooks.capacity / 2 >= g_hooks.size)
    {
        PHOOK_ENTRY p = (PHOOK_ENTRY)HeapReAlloc(
            g_hHeap, 0, g_hooks.pItems, (g_hooks.capacity / 2) * sizeof(HOOK_ENTRY));
        if (p == NULL)
            return;

        g_hooks.capacity /= 2;
        g_hooks.pItems = p;
    }
}

//-------------------------------------------------------------------------
static DWORD_PTR FindNewIP(PHOOK_ENTRY pHook, DWORD_PTR ip)
{
    UINT i;
    for (i = 0; i < pHook->nIP; ++i)
    {
        if (ip == ((DWORD_PTR)pHook->pTarget + pHook->oldIPs[i]))
            return (DWORD_PTR)pHook->pTrampoline + pHook->newIPs[i];
    }

    return 0;
}

//-------------------------------------------------------------------------
#if defined(_M_X64) || defined(__x86_64__)
    #define CONTEXT_IP(c) ((c).Rip)
#else
    #define CONTEXT_IP(c) ((c).Eip)
#endif

static MH_STATUS EnumerateThreads(PFROZEN_THREADS pThreads)
{
    MH_STATUS status = MH_OK;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry;
    if (snapshot == INVALID_HANDLE_VALUE)
        return MH_ERROR_THREAD_SUSPEND;

    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot, &entry))
    {
        if (GetLastError() != ERROR_NO_MORE_FILES)
            status = MH_ERROR_THREAD_SUSPEND;
    }
    else
    {
        do
        {
            if (entry.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD)
                && entry.th32OwnerProcessID == GetCurrentProcessId()
                && entry.th32ThreadID != GetCurrentThreadId())
            {
                if (pThreads->size == pThreads->capacity)
                {
                    UINT capacity = pThreads->capacity ? pThreads->capacity * 2 : INITIAL_THREAD_CAPACITY;
                    PFROZEN_THREAD items;
                    if (capacity < pThreads->capacity
                        || (SIZE_T)capacity > (SIZE_T)-1 / sizeof(FROZEN_THREAD))
                    {
                        status = MH_ERROR_MEMORY_ALLOC;
                        break;
                    }
                    if (pThreads->pItems == NULL)
                        items = (PFROZEN_THREAD)HeapAlloc(g_hHeap, HEAP_ZERO_MEMORY,
                            capacity * sizeof(FROZEN_THREAD));
                    else
                        items = (PFROZEN_THREAD)HeapReAlloc(g_hHeap, HEAP_ZERO_MEMORY,
                            pThreads->pItems, capacity * sizeof(FROZEN_THREAD));
                    if (items == NULL)
                    {
                        status = MH_ERROR_MEMORY_ALLOC;
                        break;
                    }
                    pThreads->pItems = items;
                    pThreads->capacity = capacity;
                }
                pThreads->pItems[pThreads->size++].id = entry.th32ThreadID;
            }
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot, &entry));
        if (status == MH_OK && GetLastError() != ERROR_NO_MORE_FILES)
            status = MH_ERROR_THREAD_SUSPEND;
    }
    CloseHandle(snapshot);
    return status;
}

static BOOL EntryMatches(PHOOK_ENTRY pHook);

static BOOL RecoveryIPMatches(DWORD_PTR ip)
{
    BOOL owned = FALSE;
    UINT i;
    for (i = 0; i < g_hooks.size; ++i)
    {
        PHOOK_ENTRY hook = &g_hooks.pItems[i];
        DWORD_PTR first = (DWORD_PTR)hook->pTarget - (hook->patchAbove ? sizeof(JMP_REL) : 0);
        // Include the jump-back boundary immediately after the relocated
        // prologue. A foreign replacement can cover that instruction as well.
        if (ip >= first && ip <= (DWORD_PTR)hook->pTarget + hook->entrySize)
        {
            if (!EntryMatches(hook))
                return FALSE;
            owned = TRUE;
        }
    }
    return owned;
}

// Never discard a handle that still owns a successful suspension. If a resume
// fails for a live thread, the next API call retries it before starting work.
static MH_STATUS Unfreeze(PFROZEN_THREADS pThreads)
{
    MH_STATUS status = MH_OK;
    UINT i;
    for (i = 0; i < pThreads->size; ++i)
    {
        PFROZEN_THREAD thread = &pThreads->pItems[i];
        if (thread->handle == NULL)
            continue;
        if (thread->contextPending
            && WaitForSingleObject(thread->handle, 0) != WAIT_OBJECT_0)
        {
            CONTEXT next = thread->current;
            if (!RecoveryIPMatches(thread->pendingIP))
            {
                status = MH_ERROR_CONFLICT;
                continue;
            }
            CONTEXT_IP(next) = thread->pendingIP;
            if (!SetThreadContext(thread->handle, &next))
            {
                status = MH_ERROR_THREAD_CONTEXT;
                continue;
            }
            thread->current = next;
            thread->contextPending = FALSE;
        }
        if (thread->suspended
            && ResumeThread(thread->handle) == (DWORD)-1
            && WaitForSingleObject(thread->handle, 0) != WAIT_OBJECT_0)
        {
            status = MH_ERROR_THREAD_SUSPEND;
            continue;
        }
        thread->suspended = FALSE;
        CloseHandle(thread->handle);
        thread->handle = NULL;
    }
    if (status == MH_OK)
    {
        if (pThreads->pItems != NULL)
            HeapFree(g_hHeap, 0, pThreads->pItems);
        ZeroMemory(pThreads, sizeof(*pThreads));
    }
    return status;
}

static MH_STATUS Freeze(PFROZEN_THREADS pThreads)
{
    MH_STATUS status;
    UINT i;
    ZeroMemory(pThreads, sizeof(*pThreads));
    status = EnumerateThreads(pThreads);
    if (status != MH_OK)
        return status;
    for (i = 0; i < pThreads->size; ++i)
    {
        PFROZEN_THREAD thread = &pThreads->pItems[i];
        thread->handle = OpenThread(THREAD_ACCESS, FALSE, thread->id);
        if (thread->handle == NULL)
        {
            // A snapshot entry may have exited. Other open failures do not
            // prove that a live thread is safe to leave running through a patch.
            if (GetLastError() == ERROR_INVALID_PARAMETER)
                continue;
            return MH_ERROR_THREAD_SUSPEND;
        }
        // Thread IDs can be reused after enumeration; an unrelated process's
        // replacement does not share these entry bytes and must not be paused.
        {
            DWORD owner = GetProcessIdOfThread(thread->handle);
            if (owner == 0)
                return MH_ERROR_THREAD_SUSPEND;
            if (owner != GetCurrentProcessId())
                continue;
        }
        if (WaitForSingleObject(thread->handle, 0) == WAIT_OBJECT_0)
            continue;
        if (SuspendThread(thread->handle) == (DWORD)-1)
        {
            if (WaitForSingleObject(thread->handle, 0) == WAIT_OBJECT_0)
                continue;
            return MH_ERROR_THREAD_SUSPEND;
        }
        thread->suspended = TRUE;
        thread->original.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread->handle, &thread->original))
            return MH_ERROR_THREAD_CONTEXT;
        thread->current = thread->original;
    }
    return MH_OK;
}

//-------------------------------------------------------------------------
// ExCamera: validate ownership while the other threads are suspended, before
// either their IPs or executable bytes change. The untouched tail matters too:
// a relocated immediate can extend beyond the five bytes MinHook overwrites.
static BOOL EntryMatches(PHOOK_ENTRY pHook)
{
    UINT8 current[32], expected[32];
    SIZE_T got;
    JMP_REL jump;
    jump.opcode = 0xE9;
    jump.operand = (UINT32)((LPBYTE)pHook->pDetour
        - ((LPBYTE)pHook->pTarget + (pHook->patchAbove ? 0 : sizeof(JMP_REL))));
    if (!ReadProcessMemory(GetCurrentProcess(), pHook->pTarget, current, pHook->entrySize, &got)
        || got != pHook->entrySize)
        return FALSE;
    memcpy(expected, pHook->entryBackup, pHook->entrySize);
    if (pHook->isEnabled)
    {
        if (pHook->patchAbove)
        {
            PJMP_REL_SHORT shortJump = (PJMP_REL_SHORT)expected;
            shortJump->opcode = 0xEB;
            shortJump->operand = (UINT8)(0 - (sizeof(JMP_REL_SHORT) + sizeof(JMP_REL)));
        }
        else
            memcpy(expected, &jump, sizeof(jump));
    }
    if (memcmp(current, expected, pHook->entrySize) != 0)
        return FALSE;
    if (pHook->patchAbove)
    {
        if (!ReadProcessMemory(GetCurrentProcess(), (LPBYTE)pHook->pTarget - sizeof(JMP_REL),
                current, sizeof(JMP_REL), &got) || got != sizeof(JMP_REL))
            return FALSE;
        if (memcmp(current, pHook->isEnabled ? (const void*)&jump : pHook->backup, sizeof(JMP_REL)) != 0)
            return FALSE;
    }
    return TRUE;
}

static MH_STATUS ValidateEntries(UINT pos, UINT action)
{
    UINT count;
    if (pos == ALL_HOOKS_POS)
    {
        pos = 0;
        count = g_hooks.size;
    }
    else
        count = pos + 1;
    for (; pos < count; ++pos)
    {
        PHOOK_ENTRY pHook = &g_hooks.pItems[pos];
        BOOL enable = action == ACTION_DISABLE ? FALSE
            : action == ACTION_ENABLE ? TRUE : pHook->queueEnable;
        if (pHook->isEnabled != enable && !EntryMatches(pHook))
            return MH_ERROR_CONFLICT;
    }
    return MH_OK;
}

static BOOL RequestedEnable(PHOOK_ENTRY hook, UINT action)
{
    return action == ACTION_DISABLE ? FALSE
        : action == ACTION_ENABLE ? TRUE : hook->queueEnable;
}

static MH_STATUS RestorePages(PPATCH_PAGE pages, UINT count)
{
    MH_STATUS status = MH_OK;
    while (count != 0)
    {
        PPATCH_PAGE page = &pages[--count];
        DWORD previous;
        if (!page->writable)
            continue;
        if (!VirtualProtect(page->address, g_pageSize, page->protection, &previous))
            status = MH_ERROR_MEMORY_PROTECT;
        else
            page->writable = FALSE;
    }
    return status;
}

static MH_STATUS RecoverPending(VOID)
{
    MH_STATUS status = MH_OK;
    if (g_pendingFlush != NULL)
    {
        UINT i;
        for (i = 0; i < g_pendingFlushCount; ++i)
            if (!FlushInstructionCache(GetCurrentProcess(),
                    g_pendingFlush[i].address, g_pendingFlush[i].size))
                status = MH_ERROR_MEMORY_FLUSH;
        if (status == MH_OK)
        {
            HeapFree(g_hHeap, 0, g_pendingFlush);
            g_pendingFlush = NULL;
            g_pendingFlushCount = 0;
        }
    }
    if (g_pendingPages != NULL)
    {
        MH_STATUS restored = RestorePages(g_pendingPages, g_pendingPageCount);
        if (restored == MH_OK)
        {
            HeapFree(g_hHeap, 0, g_pendingPages);
            g_pendingPages = NULL;
            g_pendingPageCount = 0;
        }
        if (status == MH_OK)
            status = restored;
    }
    // Do not resume threads through code whose cache flush has not completed.
    if (g_pendingFlush != NULL)
        return status;
    {
        MH_STATUS resumed = Unfreeze(&g_pendingThreads);
        if (status == MH_OK)
            status = resumed;
    }
    return status;
}

static MH_STATUS BuildPatchPlan(UINT pos, UINT action, PPATCH_ENTRY* entries,
    PUINT entryCount, PPATCH_PAGE* pages, PUINT pageCount)
{
    UINT i, first = pos == ALL_HOOKS_POS ? 0 : pos;
    UINT last = pos == ALL_HOOKS_POS ? g_hooks.size : pos + 1;
    UINT count = 0;
    *entries = NULL;
    *pages = NULL;
    *entryCount = 0;
    *pageCount = 0;
    for (i = first; i < last; ++i)
        if (g_hooks.pItems[i].isEnabled != RequestedEnable(&g_hooks.pItems[i], action))
            ++count;
    if (count == 0)
        return MH_OK;
    if ((SIZE_T)count > (SIZE_T)-1 / (2 * sizeof(PATCH_PAGE))
        || (SIZE_T)count > (SIZE_T)-1 / sizeof(PATCH_ENTRY))
        return MH_ERROR_MEMORY_ALLOC;
    *entries = (PPATCH_ENTRY)HeapAlloc(g_hHeap, HEAP_ZERO_MEMORY, count * sizeof(PATCH_ENTRY));
    *pages = (PPATCH_PAGE)HeapAlloc(g_hHeap, HEAP_ZERO_MEMORY, (SIZE_T)count * 2 * sizeof(PATCH_PAGE));
    if (*entries == NULL || *pages == NULL)
        return MH_ERROR_MEMORY_ALLOC;

    for (i = first; i < last; ++i)
    {
        PHOOK_ENTRY hook = &g_hooks.pItems[i];
        PPATCH_ENTRY entry;
        ULONG_PTR address, end;
        UINT j;
        if (hook->isEnabled == RequestedEnable(hook, action))
            continue;
        entry = &(*entries)[(*entryCount)++];
        entry->hook = hook;
        entry->address = (LPBYTE)hook->pTarget - (hook->patchAbove ? sizeof(JMP_REL) : 0);
        entry->size = sizeof(JMP_REL) + (hook->patchAbove ? sizeof(JMP_REL_SHORT) : 0);
        entry->enable = RequestedEnable(hook, action);
        for (j = 0; j + 1 < *entryCount; ++j)
        {
            PPATCH_ENTRY earlier = &(*entries)[j];
            if ((ULONG_PTR)entry->address < (ULONG_PTR)earlier->address + earlier->size
                && (ULONG_PTR)earlier->address < (ULONG_PTR)entry->address + entry->size)
                return MH_ERROR_CONFLICT;
        }
        address = (ULONG_PTR)entry->address & ~((ULONG_PTR)g_pageSize - 1);
        end = ((ULONG_PTR)entry->address + entry->size - 1) & ~((ULONG_PTR)g_pageSize - 1);
        for (;; address += g_pageSize)
        {
            for (j = 0; j < *pageCount; ++j)
                if ((*pages)[j].address == (LPVOID)address)
                    break;
            if (j == *pageCount)
                (*pages)[(*pageCount)++].address = (LPVOID)address;
            if (address == end)
                break;
        }
    }
    return MH_OK;
}

// Before committing, every changed RIP must be executable with BOTH the old
// and new entry bytes. An enable uses the retained original trampoline. A
// disable only moves a hot-patch prefix to the entry; trampoline RIPs stay put.
// Rewinding them into the entry here would be unsafe if a later API failed.
static MH_STATUS PrepareThreadIPs(PFROZEN_THREADS threads, PPATCH_ENTRY entries, UINT count)
{
    UINT i, j;
    for (i = 0; i < threads->size; ++i)
    {
        PFROZEN_THREAD thread = &threads->pItems[i];
        CONTEXT next;
        if (!thread->suspended)
            continue;
        next = thread->current;
        for (j = 0; j < count; ++j)
        {
            PPATCH_ENTRY entry = &entries[j];
            DWORD_PTR ip = 0;
            if (entry->enable)
            {
                ip = FindNewIP(entry->hook, (DWORD_PTR)CONTEXT_IP(next));
                if (ip != 0)
                    entry->hook->published = TRUE;
            }
            else if (entry->hook->patchAbove
                && CONTEXT_IP(next) == (DWORD_PTR)entry->address)
                ip = (DWORD_PTR)entry->hook->pTarget;
            if (ip != 0)
                CONTEXT_IP(next) = ip;
        }
        if (CONTEXT_IP(next) != CONTEXT_IP(thread->current))
        {
            if (!SetThreadContext(thread->handle, &next))
                return MH_ERROR_THREAD_CONTEXT;
            thread->current = next;
        }
    }
    return MH_OK;
}

static VOID RestoreThreadIPs(PFROZEN_THREADS threads)
{
    UINT i;
    for (i = 0; i < threads->size; ++i)
    {
        PFROZEN_THREAD thread = &threads->pItems[i];
        if (thread->suspended && CONTEXT_IP(thread->current) != CONTEXT_IP(thread->original))
        {
            if (SetThreadContext(thread->handle, &thread->original))
                thread->current = thread->original;
            else
            {
                thread->contextPending = TRUE;
                thread->pendingIP = (DWORD_PTR)CONTEXT_IP(thread->original);
            }
        }
    }
    // A failed restoration keeps that exact thread suspended, with its safe
    // trampoline allocated, until the original RIP can be restored. A failed
    // enable must not silently start an in-flight trampoline call that a caller
    // could then release as an unused, disabled hook.
}

static VOID WritePatch(PPATCH_ENTRY entry)
{
    PHOOK_ENTRY hook = entry->hook;
    if (entry->enable)
    {
        PJMP_REL jump = (PJMP_REL)entry->address;
        hook->published = TRUE;
        jump->opcode = 0xE9;
        jump->operand = (UINT32)((LPBYTE)hook->pDetour - (entry->address + sizeof(JMP_REL)));
        if (hook->patchAbove)
        {
            PJMP_REL_SHORT shortJump = (PJMP_REL_SHORT)hook->pTarget;
            shortJump->opcode = 0xEB;
            shortJump->operand = (UINT8)(0 - (sizeof(JMP_REL_SHORT) + sizeof(JMP_REL)));
        }
    }
    else
        memcpy(entry->address, hook->backup, entry->size);
    hook->isEnabled = entry->enable;
    hook->queueEnable = entry->enable;
}

static MH_STATUS ApplyHookChanges(UINT pos, UINT action)
{
    MH_STATUS status = RecoverPending();
    FROZEN_THREADS threads;
    PPATCH_ENTRY entries = NULL;
    PPATCH_PAGE pages = NULL;
    UINT entryCount = 0, pageCount = 0, i;
    ZeroMemory(&threads, sizeof(threads));
    if (status != MH_OK)
        return status;
    status = BuildPatchPlan(pos, action, &entries, &entryCount, &pages, &pageCount);
    if (status == MH_OK && entryCount != 0)
    {
        status = Freeze(&threads);
        if (status == MH_OK)
            status = ValidateEntries(pos, action);
        if (status == MH_OK)
        {
            // Acquire each distinct page once, for the ENTIRE batch. Besides
            // avoiding partial batches, this preserves the original protection
            // when multiple hooks share a page or a patch crosses a boundary.
            for (i = 0; i < pageCount; ++i)
            {
                if (!VirtualProtect(pages[i].address, g_pageSize,
                        PAGE_EXECUTE_READWRITE, &pages[i].protection))
                {
                    status = MH_ERROR_MEMORY_PROTECT;
                    break;
                }
                pages[i].writable = TRUE;
            }
        }
        if (status == MH_OK)
        {
            status = PrepareThreadIPs(&threads, entries, entryCount);
            if (status != MH_OK)
                RestoreThreadIPs(&threads);
        }
        if (status == MH_OK)
        {
            // No fallible OS operation remains between the first byte change
            // and the last. Publish the actual state even if later cache or
            // protection cleanup fails; teardown must retain live trampolines.
            for (i = 0; i < entryCount; ++i)
                WritePatch(&entries[i]);
            for (i = 0; i < entryCount; ++i)
                if (!FlushInstructionCache(GetCurrentProcess(), entries[i].address, entries[i].size))
                    status = MH_ERROR_MEMORY_FLUSH;
            if (status == MH_ERROR_MEMORY_FLUSH)
            {
                g_pendingFlush = entries;
                g_pendingFlushCount = entryCount;
                entries = NULL;
            }
        }
    }
    if (RestorePages(pages, pageCount) != MH_OK)
    {
        g_pendingPages = pages;
        g_pendingPageCount = pageCount;
        pages = NULL;
        if (status == MH_OK)
            status = MH_ERROR_MEMORY_PROTECT;
    }
    if (g_pendingFlush != NULL)
        g_pendingThreads = threads;
    else
    {
        MH_STATUS resumed = Unfreeze(&threads);
        if (resumed != MH_OK)
        {
            g_pendingThreads = threads;
            if (status == MH_OK)
                status = resumed;
        }
    }
    if (entries != NULL)
        HeapFree(g_hHeap, 0, entries);
    if (pages != NULL)
        HeapFree(g_hHeap, 0, pages);
    return status;
}

static MH_STATUS EnableAllHooksLL(BOOL enable)
{
    return ApplyHookChanges(ALL_HOOKS_POS, enable ? ACTION_ENABLE : ACTION_DISABLE);
}

//-------------------------------------------------------------------------
static VOID EnterSpinLock(VOID)
{
    SIZE_T spinCount = 0;

    // Wait until the flag is FALSE.
    while (InterlockedCompareExchange(&g_isLocked, TRUE, FALSE) != FALSE)
    {
        // No need to generate a memory barrier here, since InterlockedCompareExchange()
        // generates a full memory barrier itself.

        // Prevent the loop from being too busy.
        if (spinCount < 32)
            Sleep(0);
        else
            Sleep(1);

        spinCount++;
    }
}

//-------------------------------------------------------------------------
static VOID LeaveSpinLock(VOID)
{
    // No need to generate a memory barrier here, since InterlockedExchange()
    // generates a full memory barrier itself.

    InterlockedExchange(&g_isLocked, FALSE);
}

//-------------------------------------------------------------------------
BOOL WINAPI MH_HasPendingRecovery(VOID)
{
    BOOL pending;
    EnterSpinLock();
    pending = g_pendingThreads.pItems != NULL || g_pendingPages != NULL || g_pendingFlush != NULL;
    LeaveSpinLock();
    return pending;
}

MH_STATUS WINAPI MH_Recover(VOID)
{
    MH_STATUS status;
    EnterSpinLock();
    status = g_hHeap != NULL ? RecoverPending() : MH_ERROR_NOT_INITIALIZED;
    LeaveSpinLock();
    return status;
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_Initialize(VOID)
{
    MH_STATUS status = MH_OK;

    EnterSpinLock();

    if (g_hHeap == NULL)
    {
        g_hHeap = HeapCreate(0, 0, 0);
        if (g_hHeap != NULL)
        {
            SYSTEM_INFO info;
            GetSystemInfo(&info);
            g_pageSize = info.dwPageSize;
            // Initialize the internal function buffer.
            InitializeBuffer();
        }
        else
        {
            status = MH_ERROR_MEMORY_ALLOC;
        }
    }
    else
    {
        status = MH_ERROR_ALREADY_INITIALIZED;
    }

    LeaveSpinLock();

    return status;
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_Uninitialize(VOID)
{
    MH_STATUS status = MH_OK;

    EnterSpinLock();

    if (g_hHeap != NULL)
    {
        status = ApplyHookChanges(ALL_HOOKS_POS, ACTION_DISABLE);
        if (status == MH_OK)
        {
            // Only private, never-published storage can be reclaimed. A caller
            // or an in-flight return can retain every other trampoline.
            {
                UINT i;
                for (i = 0; i < g_hooks.size; ++i)
                    if (g_hooks.pItems[i].published)
                        RetireBuffer(g_hooks.pItems[i].pTrampoline);
                    else
                        FreeBuffer(g_hooks.pItems[i].pTrampoline);
            }
            UninitializeBuffer();

            HeapFree(g_hHeap, 0, g_hooks.pItems);
            HeapDestroy(g_hHeap);

            g_hHeap = NULL;

            g_hooks.pItems   = NULL;
            g_hooks.capacity = 0;
            g_hooks.size     = 0;
        }
    }
    else
    {
        status = MH_ERROR_NOT_INITIALIZED;
    }

    LeaveSpinLock();

    return status;
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID *ppOriginal)
{
    MH_STATUS status = MH_OK;

    EnterSpinLock();

    if (g_hHeap != NULL)
    {
        status = RecoverPending();
        if (status != MH_OK)
        {
            LeaveSpinLock();
            return status;
        }
    }

    if (g_hHeap != NULL)
    {
        if (IsExecutableAddress(pTarget) && IsExecutableAddress(pDetour))
        {
            UINT pos = FindHookEntry(pTarget);
            if (pos == INVALID_HOOK_POS)
            {
                LPVOID pBuffer = AllocateBuffer(pTarget);
                if (pBuffer != NULL)
                {
                    TRAMPOLINE ct;

                    ct.pTarget     = pTarget;
                    ct.pDetour     = pDetour;
                    ct.pTrampoline = pBuffer;
                    if (CreateTrampolineFunction(&ct))
                    {
                        PHOOK_ENTRY pHook = AddHookEntry();
                        if (pHook != NULL)
                        {
                            pHook->pTarget     = ct.pTarget;
#if defined(_M_X64) || defined(__x86_64__)
                            pHook->pDetour     = ct.pRelay;
#else
                            pHook->pDetour     = ct.pDetour;
#endif
                            pHook->pTrampoline = ct.pTrampoline;
                            pHook->patchAbove  = ct.patchAbove;
                            pHook->isEnabled   = FALSE;
                            pHook->queueEnable = FALSE;
                            pHook->published   = ppOriginal != NULL;
                            pHook->nIP         = ct.nIP;
                            pHook->entrySize   = ct.oldSize;
                            if (pHook->entrySize < (ct.patchAbove ? sizeof(JMP_REL_SHORT) : sizeof(JMP_REL)))
                                pHook->entrySize = ct.patchAbove ? sizeof(JMP_REL_SHORT) : sizeof(JMP_REL);
                            memcpy(pHook->entryBackup, pTarget, pHook->entrySize);
                            memcpy(pHook->oldIPs, ct.oldIPs, ARRAYSIZE(ct.oldIPs));
                            memcpy(pHook->newIPs, ct.newIPs, ARRAYSIZE(ct.newIPs));

                            // Back up the target function.

                            if (ct.patchAbove)
                            {
                                memcpy(
                                    pHook->backup,
                                    (LPBYTE)pTarget - sizeof(JMP_REL),
                                    sizeof(JMP_REL) + sizeof(JMP_REL_SHORT));
                            }
                            else
                            {
                                memcpy(pHook->backup, pTarget, sizeof(JMP_REL));
                            }

                            if (ppOriginal != NULL)
                                *ppOriginal = pHook->pTrampoline;
                        }
                        else
                        {
                            status = MH_ERROR_MEMORY_ALLOC;
                        }
                    }
                    else
                    {
                        status = MH_ERROR_UNSUPPORTED_FUNCTION;
                    }

                    if (status != MH_OK)
                    {
                        FreeBuffer(pBuffer);
                    }
                }
                else
                {
                    status = MH_ERROR_MEMORY_ALLOC;
                }
            }
            else
            {
                status = MH_ERROR_ALREADY_CREATED;
            }
        }
        else
        {
            status = MH_ERROR_NOT_EXECUTABLE;
        }
    }
    else
    {
        status = MH_ERROR_NOT_INITIALIZED;
    }

    LeaveSpinLock();

    return status;
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_RemoveHook(LPVOID pTarget)
{
    MH_STATUS status = MH_OK;

    EnterSpinLock();

    if (g_hHeap != NULL)
    {
        status = RecoverPending();
        if (status != MH_OK)
        {
            LeaveSpinLock();
            return status;
        }
    }

    if (g_hHeap != NULL)
    {
        UINT pos = FindHookEntry(pTarget);
        if (pos != INVALID_HOOK_POS)
        {
            // Complete any pending recovery before retiring the metadata. A
            // published trampoline stays allocated regardless of current RIPs.
            status = ApplyHookChanges(pos, ACTION_DISABLE);

            if (status == MH_OK)
            {
                if (g_hooks.pItems[pos].published)
                    RetireBuffer(g_hooks.pItems[pos].pTrampoline);
                else
                    FreeBuffer(g_hooks.pItems[pos].pTrampoline);
                DeleteHookEntry(pos);
            }
        }
        else
        {
            status = MH_ERROR_NOT_CREATED;
        }
    }
    else
    {
        status = MH_ERROR_NOT_INITIALIZED;
    }

    LeaveSpinLock();

    return status;
}

//-------------------------------------------------------------------------
static MH_STATUS EnableHook(LPVOID pTarget, BOOL enable)
{
    MH_STATUS status = MH_OK;

    EnterSpinLock();

    if (g_hHeap != NULL)
    {
        status = RecoverPending();
        if (status != MH_OK)
        {
            LeaveSpinLock();
            return status;
        }
    }

    if (g_hHeap != NULL)
    {
        if (pTarget == MH_ALL_HOOKS)
        {
            status = EnableAllHooksLL(enable);
        }
        else
        {
            UINT pos = FindHookEntry(pTarget);
            if (pos != INVALID_HOOK_POS)
            {
                if (g_hooks.pItems[pos].isEnabled != enable)
                {
                    status = ApplyHookChanges(pos, enable ? ACTION_ENABLE : ACTION_DISABLE);
                }
                else
                {
                    status = enable ? MH_ERROR_ENABLED : MH_ERROR_DISABLED;
                }
            }
            else
            {
                status = MH_ERROR_NOT_CREATED;
            }
        }
    }
    else
    {
        status = MH_ERROR_NOT_INITIALIZED;
    }

    LeaveSpinLock();

    return status;
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_EnableHook(LPVOID pTarget)
{
    return EnableHook(pTarget, TRUE);
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_DisableHook(LPVOID pTarget)
{
    return EnableHook(pTarget, FALSE);
}

//-------------------------------------------------------------------------
static MH_STATUS QueueHook(LPVOID pTarget, BOOL queueEnable)
{
    MH_STATUS status = MH_OK;

    EnterSpinLock();

    if (g_hHeap != NULL)
    {
        status = RecoverPending();
        if (status != MH_OK)
        {
            LeaveSpinLock();
            return status;
        }
    }

    if (g_hHeap != NULL)
    {
        if (pTarget == MH_ALL_HOOKS)
        {
            UINT i;
            for (i = 0; i < g_hooks.size; ++i)
                g_hooks.pItems[i].queueEnable = queueEnable;
        }
        else
        {
            UINT pos = FindHookEntry(pTarget);
            if (pos != INVALID_HOOK_POS)
            {
                g_hooks.pItems[pos].queueEnable = queueEnable;
            }
            else
            {
                status = MH_ERROR_NOT_CREATED;
            }
        }
    }
    else
    {
        status = MH_ERROR_NOT_INITIALIZED;
    }

    LeaveSpinLock();

    return status;
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_QueueEnableHook(LPVOID pTarget)
{
    return QueueHook(pTarget, TRUE);
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_QueueDisableHook(LPVOID pTarget)
{
    return QueueHook(pTarget, FALSE);
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_ApplyQueued(VOID)
{
    MH_STATUS status = MH_OK;

    EnterSpinLock();

    if (g_hHeap != NULL)
    {
        status = ApplyHookChanges(ALL_HOOKS_POS, ACTION_APPLY_QUEUED);
    }
    else
    {
        status = MH_ERROR_NOT_INITIALIZED;
    }

    LeaveSpinLock();

    return status;
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_CreateHookApiEx(
    LPCWSTR pszModule, LPCSTR pszProcName, LPVOID pDetour,
    LPVOID *ppOriginal, LPVOID *ppTarget)
{
    HMODULE hModule;
    LPVOID  pTarget;

    hModule = GetModuleHandleW(pszModule);
    if (hModule == NULL)
        return MH_ERROR_MODULE_NOT_FOUND;

    pTarget = (LPVOID)GetProcAddress(hModule, pszProcName);
    if (pTarget == NULL)
        return MH_ERROR_FUNCTION_NOT_FOUND;

    if (ppTarget != NULL)
        *ppTarget = pTarget;

    return MH_CreateHook(pTarget, pDetour, ppOriginal);
}

//-------------------------------------------------------------------------
MH_STATUS WINAPI MH_CreateHookApi(
    LPCWSTR pszModule, LPCSTR pszProcName, LPVOID pDetour, LPVOID *ppOriginal)
{
    return MH_CreateHookApiEx(pszModule, pszProcName, pDetour, ppOriginal, NULL);
}

//-------------------------------------------------------------------------
const char *WINAPI MH_StatusToString(MH_STATUS status)
{
#define MH_ST2STR(x)    \
    case x:             \
        return #x;

    switch (status) {
        MH_ST2STR(MH_UNKNOWN)
        MH_ST2STR(MH_OK)
        MH_ST2STR(MH_ERROR_ALREADY_INITIALIZED)
        MH_ST2STR(MH_ERROR_NOT_INITIALIZED)
        MH_ST2STR(MH_ERROR_ALREADY_CREATED)
        MH_ST2STR(MH_ERROR_NOT_CREATED)
        MH_ST2STR(MH_ERROR_ENABLED)
        MH_ST2STR(MH_ERROR_DISABLED)
        MH_ST2STR(MH_ERROR_NOT_EXECUTABLE)
        MH_ST2STR(MH_ERROR_UNSUPPORTED_FUNCTION)
        MH_ST2STR(MH_ERROR_MEMORY_ALLOC)
        MH_ST2STR(MH_ERROR_MEMORY_PROTECT)
        MH_ST2STR(MH_ERROR_MODULE_NOT_FOUND)
        MH_ST2STR(MH_ERROR_FUNCTION_NOT_FOUND)
        MH_ST2STR(MH_ERROR_CONFLICT)
        MH_ST2STR(MH_ERROR_THREAD_SUSPEND)
        MH_ST2STR(MH_ERROR_THREAD_CONTEXT)
        MH_ST2STR(MH_ERROR_MEMORY_FLUSH)
    }

#undef MH_ST2STR

    return "(unknown)";
}
