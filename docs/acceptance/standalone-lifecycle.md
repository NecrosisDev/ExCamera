# Standalone lifecycle acceptance

## Scope

Zero-hook attachment, status, stop, and restart against the real Exanima process. This is not camera or gameplay acceptance.

Game executable SHA-256:

```text
af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70
```

## Launcher commands

```powershell
ExCamera.exe --attach
ExCamera.exe --status
ExCamera.exe --stop
ExCamera.exe --self-test
```

The launcher hashes the running executable before opening it, detects an already loaded `ExanimaRuntime.dll`, resolves remote exports relative to the actual remote module base, and frees remote argument/status buffers. Stop does not unload the DLL; this avoids unsafe unload and permits a clean later restart.

## Observed live sequence

The following commands completed against the supplied standard build:

1. First attach: `state=dormant generation=1 installed_hooks=0 active_hooks=0 last_error=ok abi=1`.
2. Independent status: identical dormant state and zero hooks.
3. First stop: `state=stopped generation=1 installed_hooks=0 active_hooks=0 last_error=ok`.
4. Re-attach through the already loaded runtime: `state=dormant generation=2 installed_hooks=0 active_hooks=0 last_error=ok`.
5. Independent status: identical dormant generation 2.
6. Final stop: `state=stopped generation=2 installed_hooks=0 active_hooks=0 last_error=ok`.
7. Exanima was closed gracefully through its window.

The second start proves generation advancement and reuse of the existing loaded runtime rather than another injection. No launcher, remote worker, or analysis process was left running.

## Offline checks

- Six CTest suites pass in Release: hook broker, session lifecycle, packaged runtime, executable identity, camera math, and camera controller.
- Executable hashing matches the published SHA-256 `abc` test vector.
- Missing files fail closed.
- The packaged self-test completes eight start/stop generations with zero hooks.

## Evidence limits

- No camera hook, game-memory write, or gameplay scenario was enabled in this test.
- DLL unload, EMTK coexistence, Hellmode, and other executable builds were not tested.
- Remote-call timeout leaves the remote work's completion unknown; it is not reported as a clean stop.
- The acceptance does not establish camera, input, rendering, settings, or Lua behavior.
