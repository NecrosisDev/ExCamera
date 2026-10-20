ExCamera profiling comparison - 2026-10-20

The checked-in candidate is 0.2.2-prof8, revision 56. Profiling defaults off.
Build it with python -B build.py --package. No previous runtime ZIP is bundled
in this source repository.

For the headwear fix, compare against pre-fix 0.2.2-prof6 (revision 54).
For the original performance changes, the instrumented previous-release
comparison is 0.2.1-prof1 (revision 47, d5507b7 plus profiling only).
Use source-matched retained comparison packages; do not relabel capture JSON.
Prof6 and prof8 both report variant optimized: verify build and revision in
status/capture metadata. The helper can warn about their variant order.

Run the commands below from tools/ (python -B profile.py ...), or use
python -B tools/profile.py ... from the repository root. Match equipment,
settings, route and graphics conditions. New headwear_colour, headwear_draw
and headwear_submit spans, plus their .native spans, appear in prof8; original
metric identities are preserved. Profiler results do not establish safety or
in-game visual acceptance.

Python 3.8+ is needed only for profile.py. The mod works with profiling off.
Use your normal ExCamera loading method with one ZIP's extracted runtime.
Finish and exit Exanima normally before switching versions: stopping/re-arming
the mod does not unload its mapped DLL. Keep your existing settings and saves.

Capture a repeatable scene
--------------------------
1. Load the previous profiling build normally. Enter the test scene, activate
   the camera, and let loading/shader/asset activity settle. From this bundle's
   folder, open PowerShell and run:

     python -B profile.py status
     python -B profile.py start --label "previous-fp-wall" --seconds 60 --sample-every 64

2. Re-run `python -B profile.py status` after a second. Verify its request ID
   matches the new request and its state is capturing. Reproduce the route or
   action for 60 seconds, then check status again for complete and a report path.
   An optional early stop is `python -B profile.py stop`; its report is marked
   cancelled and should be compared only with equivalently shortened captures.

3. Exit the game normally, switch to the optimized profiling build, restart
   normally and repeat in the SAME scene, camera mode, route, resolution,
   supersampling quality, graphics, frame cap/VSync, background load and settings:

     python -B profile.py start --label "optimized-fp-wall" --seconds 60 --sample-every 64
     python -B profile.py status

4. Copy the two report paths from status. Compare previous first, optimized second:

     python -B profile.py compare "PREVIOUS-REPORT.json" "OPTIMIZED-REPORT.json" --output comparison.txt

Run three or more repetitions per build. Alternate build order if practical.
Useful separate routes: dense scenery, FP close to walls, turning in combat,
TP with actors/props between you and the camera, and dragging a held object
against a wall. Do not combine different routes or modes into one conclusion.
For rare hooks marked SPARSE, repeat longer or use --sample-every 8 (or 1 for a
short diagnosis); use the SAME rate for both builds. Changing rates changes
instrumentation overhead. Also check each profiling build with captures off to
see whether measurement itself noticeably affects the scene.

Files and controls
------------------
The real Windows Documents known-folder is used, including redirected Documents:
  Documents\ExCamera\profiling.ini          capture request, separate from preferences
  Documents\ExCamera\profiles\status.json  current build/PID/request/state/report
  Documents\ExCamera\profiles\profile-PID-REQUEST.json  one immutable named capture

No key binding or settings-panel row is added. Captures are off by default.
Start requires an alive profiling-build PID in status. Requests target that PID,
expire after 30 seconds, and are consumed once per loaded DLL. A stale request
cannot arm a later process. The worker polls at most once per second. Requests
last 5..300 seconds; root sample interval 1..4096 (default 64). Starting again
cancels/drains the old request before starting the new one. A retained/stopped
or failed camera session may report idle/pending; activate normally and issue a
fresh request. Do not treat the helper's "Requested" message as a started capture.

Use `python -B profile.py --directory "OTHER-ExCamera-SETTINGS-DIRECTORY" status`
only when deliberately overriding the helper's directory. The DLL always uses
its Windows Documents directory; the option does not redirect the DLL.

How to read the output
----------------------
- Hook/phase self time is QPC wall time after subtracting instrumented children.
  Inclusive time includes children. Native predecessor calls, camera-lock waits
  and swap.native are separate rows. swap.native includes driver/VSync waits.
  Self still contains work that has no child boundary and profiler overhead.
  Coverage includes nav, swap, mesh queue, viewport/sample projection, sector,
  terrain, input/pose/parts, picking, self masking/restoration, scene walking,
  view clearance, level tests, TP aim/hide, features, cue and held integration.
  This does not instrument every ExCamera hook or every native function.
- Calls are exact for admitted, non-dropped scopes. Timing samples are selected
  randomly by root invocation; nested scopes share that selection. Estimated
  totals weight each observed tick count by SampleEvery. Samples are correlated
  within a root; they are not independent measurements of every mesh or pick.
  n/a means no timing sample, not zero cost. Sampled maxima are observed maxima.
  SPARSE means fewer than 32 samples on either side; collect more data.
- The main comparison uses the thread with most owned game presentations.
  Other threads are shown separately and may run concurrently. Never add
  overlapping inclusive rows or thread totals to infer critical-path frame time.
- Presentation intervals are measured at the existing owned native swap gate on
  EVERY presentation, separately from sampled hook timings. Mean/FPS, approximate
  p50/p95/p99 (0.25 ms buckets) and exact observed maximum are reported. The last
  bucket overflows at 255.75 ms. These are presentation cadence, not GPU timing,
  render-only CPU time or a guarantee of displayed-frame delivery.
- Reports include build, revision, source base, variant, label/request/PID,
  view-mode counts, ten camera settings at capture start/end, sampled maxima,
  per-thread hook counters and recorder drops. Camera settings are partial;
  manually match ALL settings and game graphics. Start/end snapshots cannot
  detect a setting changed and then restored mid-capture.
- Up to 64 threads for the loaded DLL lifetime and 64 nested scopes are retained.
  Drops/errors are flagged. JSON is written only after admitted callbacks drain.
  It contains no game pointers, object names, save data or automatic uploads.

Verification and limits
-----------------------
See VERIFICATION.txt and manifest.json for exact packages, hashes and offline
checks. Captures and comparisons from fixture tests are synthetic evidence.
Neither these builds nor offline tests establish FPS improvement or unchanged
player-visible feel. No game was launched, no install changed, and no release
was published by this task. The A/B captures above provide the missing evidence.
