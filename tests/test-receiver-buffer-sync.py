import runpy
import unittest
from pathlib import Path
analyze = runpy.run_path(str(Path(__file__).parents[1] / 'scripts/analyze-receiver-buffer-sync.py'))['analyze']


class BufferSyncTest(unittest.TestCase):
    def inputs(self):
        log = '''VDONINJA_SYNC object=s now_ms=1040 audio_ssrc=1 video_ssrc=2 relative_ms=-20 current_audio_ms=340 current_video_ms=300 target_audio_ms=0 target_video_ms=320 adjusted=1
VDONINJA_TIMING timing=t event=render rtp=9000 now_ms=1030 render_ms=1300 min_ms=300
VDONINJA_TIMING timing=t event=release rtp=9000 now_ms=1030 render_ms=1300 receive_ms=1010
VDONINJA_COMPOSITOR object=c event=enqueue rtp=9000 now_us=1040000 reference_us=1300000
VDONINJA_COMPOSITOR object=c event=select rtp=9000 now_us=1290000 deadline_us=1300000 repeated=0 dropped=0
VDONINJA_TIMING timing=t event=render rtp=10500 now_ms=1050 render_ms=1332 min_ms=320
VDONINJA_TIMING timing=t event=release rtp=10500 now_ms=1050 render_ms=1332 receive_ms=1020
VDONINJA_COMPOSITOR object=c event=enqueue rtp=10500 now_us=1060000 reference_us=1332000
VDONINJA_COMPOSITOR object=c event=select rtp=10500 now_us=1320000 deadline_us=1333000 repeated=0 dropped=0'''
        encoded = {'records': [{'rtpTimestamp':rtp,'metadata':{'mimeType':'video/H264','receiveTime':time,'synchronizationSource':2}}
                               for rtp,time in [(9000,10),(10500,20)]],
                   'bufferWrites':[{'now':35,'kind':'video','requested':300,'applied':300}]}
        capture = {'records':[{'rtpTimestamp':9000,'presentedFrames':1,'presentationTime':300,'expectedDisplayTime':300,'callbackTime':300},
                              {'rtpTimestamp':10500,'presentedFrames':2,'presentationTime':333,'expectedDisplayTime':333,'callbackTime':333}]}
        return log, encoded, capture

    def test_same_frame_clocks_join_sync_step_without_calling_it_an_omission(self):
        result = analyze(*self.inputs())
        self.assertEqual(result['clock']['nativeMinusBrowserMs'],1000)
        event = result['compositor']['measuredCadenceEvents'][0]
        self.assertEqual(event['renderAfter']['min_ms'],320)
        self.assertEqual(event['nearbySyncDecisions'][0]['target_video_ms'],320)
        self.assertEqual(event['nearbyBufferWrites'][0]['nativeMs'],1035)
        self.assertEqual(result['compositor']['measuredNativeOmissions'],[])

    def test_inconsistent_clocks_are_rejected(self):
        log,encoded,capture = self.inputs()
        encoded['records'][1]['metadata']['receiveTime']=23
        with self.assertRaisesRegex(ValueError,'clock anchors differ'):
            analyze(log,encoded,capture)

    def test_unrelated_synchronizer_is_rejected(self):
        log,encoded,capture = self.inputs()
        with self.assertRaisesRegex(ValueError,'SSRC do not match'):
            analyze(log.replace('video_ssrc=2','video_ssrc=3'),encoded,capture)


if __name__ == '__main__':
    unittest.main()
