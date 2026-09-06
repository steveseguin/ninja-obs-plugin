import runpy
import unittest
from pathlib import Path

parse = runpy.run_path(str(Path(__file__).parents[1] / 'scripts/analyze-decode-scheduler.py'))['parse']


class DecodeSchedulerTest(unittest.TestCase):
    def trace(self, release=1010000):
        return f'''VDONINJA_SCHED event=select ssrc=1 metronome=1
VDONINJA_SCHED event=schedule scheduler=a rtp=90000 now_us=1000000 deadline_us=1025000 render_us=1040000
VDONINJA_SCHED event=decision scheduler=a rtp=90000 now_us=1000000 next_tick_us=1010000 immediate=0
VDONINJA_SCHED event=tick scheduler=a rtp=90000 now_us=1010000 next_tick_us=1026000 deadline_us=1025000 release=1
VDONINJA_SCHED event=release scheduler=a rtp=90000 now_us={release} deadline_us=1025000'''

    def test_early_tick_release_preserves_deadline_and_render_lead(self):
        selections, frames = parse(self.trace())
        self.assertEqual(selections[0]['metronome'], 1)
        self.assertEqual(frames[90000]['releaseMinusDeadlineMs'], -15)
        self.assertEqual(frames[90000]['renderLeadMs'], 30)
        self.assertEqual(frames[90000]['decisions'][-1]['event'], 'tick')

    def test_late_release_is_not_reported_as_headroom(self):
        _, frames = parse(self.trace(1027000))
        self.assertEqual(frames[90000]['releaseMinusDeadlineMs'], 2)
        self.assertEqual(frames[90000]['renderLeadMs'], 13)

    def test_wrong_scheduler_and_changed_deadline_rejected(self):
        with self.assertRaisesRegex(ValueError, 'matching scheduler'):
            parse(self.trace().replace('event=release scheduler=a', 'event=release scheduler=b'))
        with self.assertRaisesRegex(ValueError, 'deadline mismatch'):
            parse(self.trace().replace('now_us=1010000 deadline_us=1025000', 'now_us=1010000 deadline_us=1026000'))

    def test_repeated_rtp_and_missing_coverage_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Ambiguous'):
            parse(self.trace() + '\n' + self.trace().replace('scheduler=a', 'scheduler=b'))
        with self.assertRaisesRegex(ValueError, 'No matched'):
            parse('')


if __name__ == '__main__':
    unittest.main()
