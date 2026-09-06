import importlib.util
import pathlib
import unittest

path = pathlib.Path(__file__).resolve().parents[1] / 'scripts/analyze-receiver-timing-log.py'
spec = importlib.util.spec_from_file_location('receiver_log', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReceiverLogTests(unittest.TestCase):
    def test_excluded_frame_with_requested_buffer(self):
        result = module.analyze([
            '[123:9:ERROR] VDONINJA_TIMING controller=0xab timing=0xcd event=insert rtp=123 nack=1',
            '[123:9:ERROR] VDONINJA_TIMING timing=0xcd event=uninitialized rtp=123 now_ms=1000 min_ms=300',
        ])
        obj = result['objects'][0]
        self.assertEqual(obj['nackedInsertions'], 1)
        self.assertEqual(obj['incomingSamples'], 0)
        self.assertEqual(obj['bufferedUninitializedQueries'], 1)
        self.assertIsNone(obj['medianRenderLeadMs'])

    def test_processes_with_same_pointer_are_separate(self):
        result = module.analyze([
            '[1:9:ERROR] VDONINJA_TIMING timing=0xcd event=render rtp=123 now_ms=1000 render_ms=1300 min_ms=300',
            '[2:9:ERROR] VDONINJA_TIMING timing=0xcd event=render rtp=123 now_ms=1000 render_ms=900 min_ms=0',
        ])
        self.assertEqual([o['medianRenderLeadMs'] for o in result['objects']], [300, -100])

    def test_malformed_trace_is_not_silently_accepted(self):
        result = module.analyze(['VDONINJA_TIMING timing=0xcd event=render now_ms=missing'])
        self.assertEqual(result['malformedRecords'], 1)
        self.assertEqual(result['objects'], [])


if __name__ == '__main__':
    unittest.main()
