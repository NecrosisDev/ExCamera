"""Compile and link every ExCamera GLSL program on this PC's OpenGL driver, without the game.

The mod builds its shaders inside Exanima's GL context, so a GLSL mistake only shows in game (the
effect turns itself off and the F8 panel names it). This check pulls the same source strings out of
src/mod/view_effects.inl and src/mod/wounds_bake.inl, assembles each program exactly as the mod does
(viewPrelude + shared part + body; the skin bake's head + body), and builds them in a hidden window's
compatibility context. Windows only; it needs a GPU driver, not the game.

    python tools/check_shaders.py            # every program
    python tools/check_shaders.py -v         # also print driver warnings
"""
from __future__ import annotations

import argparse
import ctypes
import re
import sys
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VIEW = ROOT / 'src' / 'mod' / 'view_effects.inl'
BAKE = ROOT / 'src' / 'mod' / 'wounds_bake.inl'
MATERIAL_IDS = ROOT / 'src' / 'mod' / 'material_ids.inl'
HELPERS = ROOT / 'src' / 'mod' / 'wound_glsl.h'

# The view programs as view_effects.inl's effect table builds them: (name, shared part, body).
VIEW_PROGRAMS = [
    ('ambient occlusion', None, 'aoShader'),
    ('depth of field', 'dofLens', 'dofShader'),
    ('finishing pass', 'surfaceShading', 'finishShader'),
    ('occlusion composite', 'surfaceShading', 'compositeShader'),
    ('occlusion blending over frames', None, 'resolveShader'),
    ('depth-of-field composite', 'dofLens', 'dofCompositeShader'),
    ('eye adaptation meter', None, 'meterShader'),
    ('view-distance target', None, 'distanceShader'),
    ('wound marks', 'FP_WOUND_GLSL_HELPERS', 'woundShader'),
    ('fire and smoke', None, 'fireShader'),
    ('blood layer', None, 'bloodSplatShader'),
]
# The material-id replay's own two passes (material_ids.inl): (name, vertex, fragment).
MATERIAL_ID_PROGRAMS = [
    ('material ids: base fill', 'rectVertex', 'fillFragment'),
    ('material ids: layer combine', 'rectVertex', 'combineFragment'),
]
# Its id fragment links against the game's own vertex shader; here against a stand-in with the same outputs.
ID_VERTEX = {
    'vrTex': '#version 150\nin vec3 vPos;\nin vec2 vTex;\nout vec2 vrTex;\n'
             'void main() { vrTex = vTex; gl_Position = vec4(vPos, 1.0); }\n',
    'vriTex': '#version 150\nin vec3 vPos;\nin vec2 vTex;\nout vec2 vriTex;\n'
              'void main() { vriTex = vTex; gl_Position = vec4(vPos, 1.0); }\n',
}
BAKE_PROGRAMS = [
    ('wound skin (diffuse)', 'woundSkinFragment'),
    ('wound skin (normal)', 'woundSkinNormalFragment'),
    ('wound skin (spec)', 'woundSkinSpecFragment'),
]

_RAW = re.compile(r'R"(?P<delim>[^(\s]*)\((?P<body>.*?)\)(?P=delim)"', re.S)
_TOKEN = re.compile(r'\s*(?:(R"[^(\s]*\()|([A-Za-z_]\w*)|(;))', re.S)


def _concatenation(text: str, start: int, macros: dict[str, str]) -> str:
    """The string a C++ initializer of raw literals and macro names starting at start adds up to."""
    out, pos = [], start
    while True:
        m = _TOKEN.match(text, pos)
        if not m:
            raise ValueError(f'unexpected text at {text[pos:pos + 40]!r}')
        if m.group(3):
            return ''.join(out)
        if m.group(2):
            out.append(macros[m.group(2)])
            pos = m.end()
            continue
        raw = _RAW.match(text, m.start(1))
        out.append(raw.group('body'))
        pos = raw.end()


def collect_sources() -> dict[str, str]:
    macros: dict[str, str] = {}
    helpers = HELPERS.read_text(encoding='utf-8')
    for m in re.finditer(r'#define\s+(\w+)\s+(?=R")', helpers):
        macros[m.group(1)] = _concatenation(helpers + ';', m.end(), macros)
    sources = dict(macros)
    for path in (VIEW, BAKE, MATERIAL_IDS):
        if not path.exists():
            continue
        text = path.read_text(encoding='utf-8')
        for m in re.finditer(r'static const char\* const (\w+) = (?=R")', text):
            sources[m.group(1)] = _concatenation(text, m.end(), macros)
    return sources


# --- a hidden window with a compatibility GL context (ctypes, no packages) ---
user32, gdi32, opengl32, kernel32 = ctypes.windll.user32, ctypes.windll.gdi32, ctypes.windll.opengl32, ctypes.windll.kernel32


class PIXELFORMATDESCRIPTOR(ctypes.Structure):
    _fields_ = [('nSize', wintypes.WORD), ('nVersion', wintypes.WORD), ('dwFlags', wintypes.DWORD),
                ('iPixelType', ctypes.c_ubyte), ('cColorBits', ctypes.c_ubyte)] + \
               [(f'r{i}', ctypes.c_ubyte) for i in range(13)] + \
               [('cDepthBits', ctypes.c_ubyte), ('cStencilBits', ctypes.c_ubyte), ('cAuxBuffers', ctypes.c_ubyte),
                ('iLayerType', ctypes.c_ubyte), ('bReserved', ctypes.c_ubyte), ('dwLayerMask', wintypes.DWORD),
                ('dwVisibleMask', wintypes.DWORD), ('dwDamageMask', wintypes.DWORD)]


GL_FRAGMENT_SHADER, GL_VERTEX_SHADER = 0x8B30, 0x8B31
GL_COMPILE_STATUS, GL_LINK_STATUS, GL_INFO_LOG_LENGTH = 0x8B81, 0x8B82, 0x8B84
GL_VENDOR, GL_RENDERER, GL_VERSION = 0x1F00, 0x1F01, 0x1F02


class Gl:
    def __init__(self):
        user32.CreateWindowExW.restype = wintypes.HWND
        user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                           ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                           wintypes.HWND, ctypes.c_void_p, wintypes.HINSTANCE, wintypes.LPVOID]
        user32.GetDC.restype = wintypes.HDC
        user32.GetDC.argtypes = [wintypes.HWND]
        gdi32.ChoosePixelFormat.argtypes = [wintypes.HDC, ctypes.POINTER(PIXELFORMATDESCRIPTOR)]
        gdi32.SetPixelFormat.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.POINTER(PIXELFORMATDESCRIPTOR)]
        opengl32.wglCreateContext.restype = ctypes.c_void_p
        opengl32.wglCreateContext.argtypes = [wintypes.HDC]
        opengl32.wglMakeCurrent.argtypes = [wintypes.HDC, ctypes.c_void_p]
        opengl32.wglDeleteContext.argtypes = [ctypes.c_void_p]
        user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]  # 64-bit handles
        user32.DestroyWindow.argtypes = [wintypes.HWND]
        opengl32.wglGetProcAddress.restype = ctypes.c_void_p
        opengl32.wglGetProcAddress.argtypes = [ctypes.c_char_p]
        opengl32.glGetString.restype = ctypes.c_char_p
        self.hwnd = user32.CreateWindowExW(0, 'STATIC', 'ExCamera shader check', 0, 0, 0, 16, 16, None, None, None, None)
        if not self.hwnd:
            raise RuntimeError('could not create a hidden window')
        self.hdc = user32.GetDC(self.hwnd)
        pfd = PIXELFORMATDESCRIPTOR(nSize=ctypes.sizeof(PIXELFORMATDESCRIPTOR), nVersion=1,
                                    dwFlags=0x4 | 0x20, iPixelType=0, cColorBits=32, cDepthBits=24)
        fmt = gdi32.ChoosePixelFormat(self.hdc, ctypes.byref(pfd))
        if not fmt or not gdi32.SetPixelFormat(self.hdc, fmt, ctypes.byref(pfd)):
            raise RuntimeError('no OpenGL pixel format')
        self.hglrc = opengl32.wglCreateContext(self.hdc)  # a legacy context: the compatibility profile
        if not self.hglrc or not opengl32.wglMakeCurrent(self.hdc, self.hglrc):
            raise RuntimeError('could not make an OpenGL context current')
        u, i, sz, p = ctypes.c_uint, ctypes.c_int, ctypes.c_ssize_t, ctypes.c_void_p
        self.createShader = self._fn('glCreateShader', u, u)
        self.shaderSource = self._fn('glShaderSource', None, u, i, ctypes.POINTER(ctypes.c_char_p), p)
        self.compileShader = self._fn('glCompileShader', None, u)
        self.getShaderiv = self._fn('glGetShaderiv', None, u, u, ctypes.POINTER(i))
        self.getShaderInfoLog = self._fn('glGetShaderInfoLog', None, u, i, p, ctypes.c_char_p)
        self.deleteShader = self._fn('glDeleteShader', None, u)
        self.createProgram = self._fn('glCreateProgram', u)
        self.attachShader = self._fn('glAttachShader', None, u, u)
        self.bindAttribLocation = self._fn('glBindAttribLocation', None, u, u, ctypes.c_char_p)
        self.linkProgram = self._fn('glLinkProgram', None, u)
        self.getProgramiv = self._fn('glGetProgramiv', None, u, u, ctypes.POINTER(i))
        self.getProgramInfoLog = self._fn('glGetProgramInfoLog', None, u, i, p, ctypes.c_char_p)
        self.deleteProgram = self._fn('glDeleteProgram', None, u)
        del sz

    def _fn(self, name, restype, *argtypes):
        address = opengl32.wglGetProcAddress(name.encode())
        if not address:
            raise RuntimeError(f'{name} is not available (OpenGL 2.0 or later is needed)')
        return ctypes.WINFUNCTYPE(restype, *argtypes)(address)

    def describe(self) -> str:
        return ' / '.join((opengl32.glGetString(e) or b'?').decode(errors='replace') for e in (GL_VENDOR, GL_RENDERER, GL_VERSION))

    def _log(self, getter, lengthGetter, obj) -> str:
        length = ctypes.c_int(0)
        lengthGetter(obj, GL_INFO_LOG_LENGTH, ctypes.byref(length))
        if length.value <= 1:
            return ''
        buf = ctypes.create_string_buffer(length.value)
        getter(obj, length.value, None, buf)
        return buf.value.decode(errors='replace').strip()

    def compile(self, kind: int, parts: list[str]) -> tuple[int, str]:
        shader = self.createShader(kind)
        encoded = [s.encode('utf-8') for s in parts]
        array = (ctypes.c_char_p * len(encoded))(*encoded)
        self.shaderSource(shader, len(encoded), array, None)
        self.compileShader(shader)
        ok = ctypes.c_int(0)
        self.getShaderiv(shader, GL_COMPILE_STATUS, ctypes.byref(ok))
        log = self._log(self.getShaderInfoLog, self.getShaderiv, shader)
        if not ok.value:
            self.deleteShader(shader)
            return 0, log
        return shader, log

    def link(self, vertex: int, fragment: int) -> tuple[bool, str]:
        program = self.createProgram()
        self.attachShader(program, vertex)
        self.attachShader(program, fragment)
        self.bindAttribLocation(program, 0, b'position')
        self.linkProgram(program)
        ok = ctypes.c_int(0)
        self.getProgramiv(program, GL_LINK_STATUS, ctypes.byref(ok))
        log = self._log(self.getProgramInfoLog, self.getProgramiv, program)
        self.deleteProgram(program)
        return bool(ok.value), log

    def close(self):
        opengl32.wglMakeCurrent(None, None)
        opengl32.wglDeleteContext(self.hglrc)
        user32.ReleaseDC(self.hwnd, self.hdc)
        user32.DestroyWindow(self.hwnd)


def _with_lines(parts: list[str], log: str) -> str:
    """The driver's log, with the source line each error points at (NVIDIA/AMD/Intel all report 0(N) or 0:N)."""
    lines = ''.join(parts).split('\n')
    out = []
    for entry in log.splitlines():
        out.append('    ' + entry)
        m = re.match(r'\s*(?:ERROR:\s*)?\d+[(:](\d+)', entry)
        if m and 0 < int(m.group(1)) <= len(lines):
            out.append('      > ' + lines[int(m.group(1)) - 1].strip())
    return '\n'.join(out)


_SMOOTHSTEP = re.compile(r'smoothstep\(\s*(-?\d+(?:\.\d*)?)\s*,\s*(-?\d+(?:\.\d*)?)\s*,')


def reversed_smoothsteps(sources: dict[str, str]) -> list[str]:
    """smoothstep calls whose literal edges run backwards: GLSL leaves edge0 >= edge1 undefined, and while
    NVIDIA returns the mirrored curve, other drivers need not. Written 1.0 - smoothstep(b, a, x) instead."""
    found = []
    for name, text in sources.items():
        for m in _SMOOTHSTEP.finditer(text):
            if float(m.group(1)) >= float(m.group(2)):
                line = text.count('\n', 0, m.start()) + 1
                found.append(f'{name} line {line}: {text.splitlines()[line - 1].strip()}')
    return found


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('-v', '--verbose', action='store_true', help='print driver warnings for programs that pass')
    args = parser.parse_args(argv)
    sources = collect_sources()
    gl = Gl()
    print(f'Driver: {gl.describe()}')
    # A program this branch does not have yet (another thread's) is skipped, not failed.
    for name, shared, body in VIEW_PROGRAMS:
        if body not in sources:
            print(f'[skip] {name}: {body} is not in this source tree')
    jobs = [(name, [sources['viewVertexShader']], [sources['viewPrelude'], sources[shared] if shared else '', sources[body]])
            for name, shared, body in VIEW_PROGRAMS if body in sources]
    jobs += [(name, [sources['woundSkinVertex']], [sources['woundSkinFragmentHead'], sources[body]])
             for name, body in BAKE_PROGRAMS]
    jobs += [(name, [sources[vertex]], [sources[fragment]])
             for name, vertex, fragment in MATERIAL_ID_PROGRAMS if fragment in sources]
    if MATERIAL_IDS.exists():
        text = MATERIAL_IDS.read_text(encoding='utf-8')
        start = text.find('idFragment[2] = {')
        for m in _RAW.finditer(text, start, text.find('};', start)) if start >= 0 else ():
            varying = 'vriTex' if 'in vec2 vriTex' in m.group('body') else 'vrTex'
            jobs.append((f'material ids: id at {varying}', [ID_VERTEX[varying]], [m.group('body')]))
    failed = 0
    for name, vertexParts, fragmentParts in jobs:
        vertex, vlog = gl.compile(GL_VERTEX_SHADER, vertexParts)
        fragment, flog = gl.compile(GL_FRAGMENT_SHADER, fragmentParts) if vertex else (0, '')
        linked, llog = gl.link(vertex, fragment) if fragment else (False, '')
        if vertex:
            gl.deleteShader(vertex)
        if fragment:
            gl.deleteShader(fragment)
        if linked:
            print(f'[ok] {name}')
            if args.verbose:
                for log, parts in ((vlog, vertexParts), (flog, fragmentParts), (llog, [])):
                    if log:
                        print(_with_lines(parts, log))
            continue
        failed += 1
        print(f'[FAIL] {name}')
        if not vertex:
            print(_with_lines(vertexParts, vlog or 'vertex shader did not compile'))
        elif not fragment:
            print(_with_lines(fragmentParts, flog or 'fragment shader did not compile'))
        else:
            print(_with_lines([], llog or 'program did not link'))
    gl.close()
    for warning in reversed_smoothsteps(sources):
        print(f'[warn] reversed smoothstep edges (undefined in GLSL): {warning}')
    print(f'Result: {len(jobs) - failed}/{len(jobs)} programs build')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
