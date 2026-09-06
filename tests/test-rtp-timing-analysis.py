import importlib.util
from pathlib import Path
import unittest
import tempfile
spec = importlib.util.spec_from_file_location('timing', Path(__file__).resolve().parents[1] / 'scripts/analyze-rtp-timing.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def row(time, kind, sequence, timestamp=0):
    return {'steady_ns':time,'kind':kind,'sequence':sequence,'rtp_timestamp':timestamp}

class TimingCoverageTest(unittest.TestCase):
    def test_old_trace_does_not_claim_nack_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / 'trace.csv'
            file.write_text('steady_ns,kind,sequence,rtp_timestamp\n1,96,1,3000\n# overflow=0\n')
            rows, available = module.read_native_trace(file)
            self.assertFalse(available)
            self.assertEqual(len(rows), 1)
            file.write_text('# version=2 nack_capture=1\n' + file.read_text())
            self.assertTrue(module.read_native_trace(file)[1])

    def test_incomplete_or_overflowed_trace_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / 'trace.csv'
            for contents in ['', 'steady_ns,kind\n', '# overflow=1\n']:
                file.write_text(contents)
                with self.assertRaises(ValueError):
                    module.read_native_trace(file)

    def test_all_received_frames_are_nacked(self):
        rows=[row(1,96,10,3000),row(2,96,11,6000),row(3,205,10),row(4,205,11)]
        result=module.nack_coverage(rows,[3000,6000])
        self.assertTrue(result['allReceivedFramesNacked'])
        self.assertEqual(result['receivedFramesWithoutNack'],0)

    def test_initial_clean_frame_is_visible_and_unsampled_tail_is_excluded(self):
        rows=[row(1,96,10,3000),row(2,96,11,6000),row(3,205,11),row(4,96,12,9000)]
        result=module.nack_coverage(rows,[3000,6000])
        self.assertFalse(result['allReceivedFramesNacked'])
        self.assertEqual(result['firstCleanTimestamps'],[3000])

    def test_sequence_reuse_does_not_assign_an_old_nack_to_a_new_frame(self):
        rows=[row(1,96,65535,3000),row(2,205,65535),row(3,96,0,6000),row(4,96,65535,9000)]
        result=module.nack_coverage(rows,[3000,6000,9000])
        self.assertEqual(result['firstCleanTimestamps'],[6000,9000])

if __name__ == '__main__':
    unittest.main()
