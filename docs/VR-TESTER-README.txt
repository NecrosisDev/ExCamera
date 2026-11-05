ExCamera VR tester build - PRIVATE TEST BUILD, NOT A RELEASE

Version {version}, build {commit}
Please don't share this ZIP or post it publicly. Round 1 is about one thing:
does the game run well in a headset, and what breaks.

WHAT YOU NEED
- Exanima 0.9.5.2g from Steam (standard or Hellmode), Windows 10 or 11, 64-bit.
- A PC VR headset with two controllers, and its OpenXR runtime set as the
  active one (SteamVR, Meta/Oculus, Virtual Desktop, WMR all provide one).

INSTALL
1. Extract this ZIP into a new, empty folder. Don't mix it with a normal
   ExCamera folder. Keep every file together: openxr_loader.dll must stay
   beside ExanimaFirstPerson.dll.
2. Start your headset and its runtime (for example SteamVR) first.
3. Run ExCamera.exe from this folder. It starts Exanima through Steam.
4. In the game press F8 to open Settings, go to the VR page and set
   "VR: headset" to On. Apply, quit the game completely, and run
   ExCamera.exe again. The headset only starts when the game starts.
Your settings live in Documents\ExCamera\first-person.ini and are shared
with a normal ExCamera install. To go back to a flat screen, set
"VR: headset" to Off and restart the game.
Leave the rows marked "experimental" off for round 1.

CONTROLS
Menu button      Tap: the game's menu. Hold 1.5 s: recenter the view.
Right trigger    Attack; on a menu or wrist line, click.
Left trigger     Hold to make the attack a stab.
Grip (squeeze)   Pick up and hold; on a menu it is the right mouse button.
Left stick       Walk.
Right stick      Turn (sideways). Push forward to aim a teleport, let go to jump there.
Wrist menu       Raise your left wrist and turn your palm or its back to your
                 face. Point at a line with the right hand and pull the trigger.
Hip pouch        Let go of an item at your left hip to store it; close an
                 empty hand there to take the last one back. Needs ExanimaLua
                 (ScriptRuntime) installed; without it nothing happens.

ROUND 1 CHECKLIST
Tick each line and write a short note for anything odd, even if it "works".
[ ] 1. The game starts in the headset and the title screen shows in 3D.
[ ] 2. Menus sit in your left hand, and the right-hand pointer clicks them.
[ ] 3. Every wrist menu line works: Recenter, Posture, Turning, Hands, Body,
       Comfort, Haptics, Save report.
[ ] 4. Holding the menu button recenters the view.
[ ] 5. Walking, snap turn, smooth turn (wrist menu: Turning) and teleport all work.
[ ] 6. You can pick up and drop an item, and use the hip pouch.
[ ] 7. Fighting with the trigger works, and you feel haptics both when you
       hit and when you are hit.
[ ] 8. A 15-minute session with zero faults (see below).

WHAT TO SEND BACK
1. A report ZIP. Wrist menu, last line "Save report": a file named
   ExCamera-report-<date>-<time>.zip appears on your Desktop. Save one right
   after anything goes wrong, and one at the end of every session.
2. If the game crashed or the wrist menu was out of reach, send these files
   instead: first-person-status.json and fault-log.txt (if there is one) from
   this folder, and Documents\ExCamera\first-person.ini.
3. The checklist above with your notes, plus your headset, its runtime
   (SteamVR, Meta, ...), your graphics card, and whether you play seated
   or standing.

"Zero faults" means the report's first-person-status.json shows every
*_faults count as 0 and faults_seen as 0. You don't need to check that
yourself; we will.

IF SOMETHING GOES WRONG
Hold F7 for a second to hand the camera back to the game. If the view is
stuck or black in the headset, take it off, hold F7, then quit the game.
