import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('compositor', Path(__file__).resolve().parents[1] / 'scripts/analyze-compositor-timing.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class CompositorTimingTest(unittest.TestCase):
    def test_omission_after_decode_is_retained(self):
        lines = []
        for rtp in [90000, 91500, 93000]:
            lines += [f'VDONINJA_TIMING timing=0x1 event=release rtp={rtp} render_ms=1000',
                      f'VDONINJA_COMPOSITOR object=0x2 event=enqueue rtp={rtp} reference_us=1000000']
        for rtp in [90000, 93000]:
            lines.append(f'VDONINJA_COMPOSITOR object=0x2 event=select rtp={rtp} dropped=0 repeated=0')
        result = m.analyze('\n'.join(lines), {'records': []}, 60)
        missing = result['nativeOmissions'][0]
        self.assertEqual(missing['missingRtp'], [91500])
        self.assertEqual(missing['missingReleased'], [True])
        self.assertEqual(missing['missingEnqueued'], [True])
        self.assertEqual(result['nativeDropped'], 0)  # Do not infer a counter that was not logged.

    def test_same_pointer_in_different_processes_is_not_joined(self):
        text = '\n'.join([
            '[1:9:ERROR] VDONINJA_TIMING timing=0x1 event=release rtp=90000 render_ms=1000',
            '[2:9:ERROR] VDONINJA_COMPOSITOR object=0x2 event=enqueue rtp=90000 reference_us=1000000',
        ])
        with self.assertRaisesRegex(ValueError, 'different browser processes'):
            m.analyze(text, {'records': []}, 60)

    def test_missing_native_coverage_rejected(self):
        with self.assertRaises(ValueError):
            m.analyze('', {'records': []}, 60)


if __name__ == '__main__':
    unittest.main()
