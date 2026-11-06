import json
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import jump_bake  # noqa: E402


class JumpBakeTests(unittest.TestCase):
    def test_header_matches_the_baked_tracks(self):
        self.assertEqual(jump_bake.check_header(), 2)

    def test_rebaking_the_sources_gives_the_committed_tracks(self):
        committed = json.loads(jump_bake.TRACKS.read_text(encoding='utf-8'))
        for (name, stem), clip in zip(jump_bake.CLIPS, committed['clips']):
            fresh = jump_bake.bake(json.loads((jump_bake.SOURCES / (stem + '.json')).read_text(encoding='utf-8')), name)
            self.assertEqual(fresh, clip)
        self.assertEqual(jump_bake.max_tuck(committed['clips'][0]), committed['standing_tuck_default'])

    def test_phase_frames_come_from_the_data(self):
        clips = {c['name']: c for c in json.loads(jump_bake.TRACKS.read_text(encoding='utf-8'))['clips']}
        s, r = clips['jumping'], clips['runningJump']
        self.assertEqual((s['takeoff'], s['peak'], s['touchdown'], s['end'], s['landing_dip']), (10, 23, 33, 50, 39))
        self.assertEqual((r['takeoff'], r['peak'], r['touchdown'], r['end']), (0, 10, 18, 26))

    def test_phases_on_a_made_up_hop(self):
        hips = [90, 80, 70, 80, 100, 120, 100, 80, 70, 85, 90, 90]
        up = [[0, 0]] * 4 + [[20, 20], [40, 40], [20, 20]] + [[0, 0]] * 5
        p = jump_bake.phases(hips, up, 0)
        self.assertEqual((p['takeoff'], p['peak'], p['touchdown'], p['end'], p['landing_dip']), (2, 5, 7, 10, 8))

    def test_tuck_default_keeps_every_flight_frame_in_the_margin(self):
        clip = json.loads(jump_bake.TRACKS.read_text(encoding='utf-8'))['clips'][0]
        tuck = jump_bake.max_tuck(clip)
        frames = range(clip['takeoff'], clip['touchdown'] + 1)
        self.assertTrue(0 < tuck < 1)
        self.assertLessEqual(max(jump_bake.foot_clearance(clip, f, tuck, 15) for f in frames), 20)
        self.assertGreater(max(jump_bake.foot_clearance(clip, f, tuck + 0.01, 15) for f in frames), 20)


if __name__ == '__main__':
    unittest.main()
