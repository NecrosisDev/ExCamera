; 0x18C70 preserves every general-purpose register. Keep that stronger native
; contract while C++ prepares a local projection, then let the native SIMD
; multiply produce both its matrix output and its volatile XMM register state.
option casemap:none

EXTERN excameraPrepareSampleProjection:PROC
EXTERN excameraOriginalMultiplyMatrix:QWORD
PUBLIC excameraMultiplyMatrixHook

.code
excameraMultiplyMatrixHook PROC FRAME
    sub rsp, 098h
    .allocstack 098h
    .endprolog

    mov qword ptr [rsp+060h], rax
    mov qword ptr [rsp+068h], rcx
    mov qword ptr [rsp+070h], rdx
    mov qword ptr [rsp+078h], r8
    mov qword ptr [rsp+080h], r9
    mov qword ptr [rsp+088h], r10
    mov qword ptr [rsp+090h], r11

    ; The first 32 bytes are the Windows x64 call home area. The next 64
    ; bytes hold this invocation's projection and cannot escape this call.
    lea r9, [rsp+020h]
    call excameraPrepareSampleProjection
    test al, al
    jz native_projection
    lea r8, [rsp+020h]
    jmp multiply
native_projection:
    mov r8, qword ptr [rsp+078h]
multiply:
    mov rcx, qword ptr [rsp+068h]
    mov rdx, qword ptr [rsp+070h]
    call qword ptr [excameraOriginalMultiplyMatrix]

    mov rax, qword ptr [rsp+060h]
    mov rcx, qword ptr [rsp+068h]
    mov rdx, qword ptr [rsp+070h]
    mov r8, qword ptr [rsp+078h]
    mov r9, qword ptr [rsp+080h]
    mov r10, qword ptr [rsp+088h]
    mov r11, qword ptr [rsp+090h]
    add rsp, 098h
    ret
excameraMultiplyMatrixHook ENDP
END
