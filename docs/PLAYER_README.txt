ExCamera: first- and third-person camera for Exanima

Works with Exanima 0.9.5.2g on Windows (standard and Hellmode executables).
On other game versions it shows a warning, or refuses to start and changes nothing.

INSTALL
1. Extract this folder anywhere. Keep its files together.
2. Run ExCamera.exe. It starts Exanima through Steam if the game isn't running,
   or attaches to a running game. A short message in game confirms it started.
Run ExCamera.exe each time you play. To remove the mod, delete
this folder and Documents\ExCamera.

KEYS & CONTROLS
F6            Switch between first person, third person and the game's camera
              (Settings > Camera > F6 cycle "1st/3rd only": a tap flips first and
              third person, holding F6 half a second shows the game's camera)
F7 (hold 1s)  Stop the mod
F8            Open Settings and free the cursor; press again to close without saving
              (first person, third person and the title screen only)
F9 / F10      Lower / raise the field of view
F11           Reset the field of view
F12           Toggle Live Telemetry & Wound Debug Mode (neon highlight for wounds/blood)
Cursor Look   The game's own binding (Space by default): free the cursor while exploring.
              After an arena fight you keep looking around; press it to click Next Fight
Hold RMB      (In combat) Engage Manual Guard to actively angle your parry against attacks
Hold RMB      (While dragging an object with LMB) Rotate the held object in 3D pitch and yaw
Mouse Wheel   In third person, adjust camera distance; while dragging objects, adjust reach

Settings are in the game's Settings window. Your preferences are saved in
Documents\ExCamera\first-person.ini and survive updates.

FIRST-PERSON AIM & COMBAT
The mouse moves your aim inside a soft oval, and the view follows as you push
toward its edge. A middle click centres the view on your aim. Directional
swings follow footwork and mouse sweeps. Holding RMB with a drawn weapon activates
active manual guard.

IN-GAME MANUAL
Open the native MANUAL tab on the main menu to read the complete First Person (ExCamera)
in-game guide page anytime.

IF SOMETHING GOES WRONG
Hold F7 to hand the camera back to the game. Hold Shift while starting
ExCamera.exe to start with default settings; your current settings are
kept as first-person.backup-<date>-<time>.ini beside them. Close the game fully before
updating or replacing these files. When reporting a problem, include
first-person-status.json and fault-log.txt from this folder.

Wound marks (cuts, bruises, punctures, and weapon blood) are fully integrated
and run standalone without external dependencies.

License: MIT (LICENSE.txt). Bundled MinHook: LICENSE-MinHook.txt.
No game files are included.
