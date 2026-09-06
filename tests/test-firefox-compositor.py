import runpy
import unittest
from pathlib import Path

analyze = runpy.run_path(str(Path(__file__).parents[1] / 'scripts/analyze-firefox-compositor.py'))['analyze']


class FirefoxCompositorTest(unittest.TestCase):
    def inputs(self):
        log = '''[Child 42: Main] VDONINJA_FF_COMPOSITOR event=enqueue object=c producer=1 frame=1 rtp=90000 now_us=970000 reference_us=1000000
[Child 42: Main] VDONINJA_FF_COMPOSITOR event=enqueue object=c producer=1 frame=2 rtp=91500 now_us=990000 reference_us=1016667
[Child 42: Main] VDONINJA_FF_COMPOSITOR event=composite object=c producer=1 frame=1 now_us=1040000 composite_us=1000000 reference_us=1000000 paint_count=1
[Child 42: Main] VDONINJA_FF_COMPOSITOR event=composite object=c producer=1 frame=2 now_us=1073000 composite_us=1016667 reference_us=1016667 paint_count=2'''
        capture = {'records': [{'rtpTimestamp': 90000, 'presentedFrames': 1, 'presentationTime': 1000},
                               {'rtpTimestamp': 91500, 'presentedFrames': 2, 'presentationTime': 1033}]}
        return log, capture

    def test_notification_delivery_delay_is_not_called_a_composition_stall(self):
        result = analyze(*self.inputs())
        self.assertTrue(result['ok'])
        self.assertEqual(result['matchedMeasuredRecords'], 2)
        self.assertEqual(result['measuredCadenceEvents'][0]['submissionIntervalMs'], 33)
        self.assertAlmostEqual(result['measuredCadenceEvents'][0]['nativeCompositionIntervalMs'], 16.667)

    def test_actual_composition_stall_is_retained(self):
        log, capture = self.inputs()
        result = analyze(log.replace('composite_us=1016667', 'composite_us=1033333'), capture)
        self.assertFalse(result['ok'])
        self.assertAlmostEqual(result['measuredCadenceEvents'][0]['nativeCompositionIntervalMs'], 33.333)

    def test_queued_but_uncomposited_frame_is_not_hidden_by_callback_coverage(self):
        log, capture = self.inputs()
        log = log.replace('frame=2', 'frame=3').replace('rtp=91500', 'rtp=93000')
        log += '\n[Child 42: Main] VDONINJA_FF_COMPOSITOR event=enqueue object=c producer=1 frame=2 rtp=91500 now_us=980000 reference_us=1010000'
        capture['records'][1]['rtpTimestamp'] = 93000
        result = analyze(log, capture)
        self.assertEqual(result['matchedMeasuredRecords'], 2)
        self.assertFalse(result['ok'])
        self.assertEqual(result['nativeOmittedFrames'], 1)
        self.assertEqual(result['nativeOmissions'][0]['missingQueued'], [True])

    def test_missing_and_cross_process_coverage_rejected(self):
        log, capture = self.inputs()
        with self.assertRaisesRegex(ValueError, 'combine Firefox processes'):
            analyze(log.replace('[Child 42: Main] VDONINJA_FF_COMPOSITOR event=composite',
                                '[Child 43: Main] VDONINJA_FF_COMPOSITOR event=composite'), capture)
        with self.assertRaisesRegex(ValueError, 'No measured RTP'):
            analyze(log.replace('producer=1 frame=2 now_us=', 'producer=2 frame=2 now_us='),
                    {'records': [capture['records'][1]]})


if __name__ == '__main__':
    unittest.main()
