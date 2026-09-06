import importlib.util
import math
import pathlib
import unittest

path = pathlib.Path(__file__).resolve().parents[1]/'scripts/analyze-av-playout.py'
spec = importlib.util.spec_from_file_location('av_playout',path)
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)


class PulseTests(unittest.TestCase):
    def test_unique_source_ids_survive_output_gain(self):
        rate = 48000
        for gain in (0.2, 1):
            samples = []
            for n in range(rate*6):
                t=n/rate; pulse=int(t//2)
                value=12000*math.sin(2*math.pi*(997+100*pulse)*t) if t%2<0.1 else 360*math.sin(2*math.pi*997*t)
                samples.append(round(value*gain))
            pulses,rejected=module.detect_pulses(samples,rate)
            self.assertEqual([p['sourceFrame'] for p in pulses],[0,60,120])
            self.assertEqual(rejected,[])
            self.assertTrue(all(abs(p['sample']-i*2*rate)<=rate//200 for i,p in enumerate(pulses)))

    def test_silence_cannot_establish_alignment(self):
        pulses,rejected=module.detect_pulses([0]*48000,48000)
        self.assertEqual(pulses,[])
        self.assertTrue(rejected)


if __name__=='__main__': unittest.main()
