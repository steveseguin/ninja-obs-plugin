#!/usr/bin/env python3
"""Match uniquely pitched synthetic pulses to compositor frame markers."""
import argparse
import array
import bisect
import json
import math
import pathlib
import statistics
import subprocess
import sys


def detect_pulses(samples, rate):
    block = rate // 200  # 5 ms; fixed before any browser measurements.
    rms = [math.sqrt(sum(x*x for x in samples[i:i+block]) / len(samples[i:i+block]))
           for i in range(0, len(samples), block)]
    if not rms or max(rms) < 32:
        return [], [{'sample': 0, 'reason': 'No sufficiently audible pulses'}]
    threshold = sorted(rms)[int(0.99*(len(rms)-1))] * 0.5
    active = None
    pulses, rejected = [], []
    for i, level in enumerate(rms + [0]):
        if level >= threshold and active is None:
            active = i
        elif level < threshold and active is not None:
            start, end = active * block, i * block
            active = None
            if not 0.05*rate <= end-start <= 0.16*rate:
                rejected.append({'sample': start, 'reason': 'Pulse duration outside 50–160 ms'})
                continue
            crossings = []
            for n in range(start + rate//100, min(end - rate//100, len(samples))):
                if samples[n-1] <= 0 < samples[n]:
                    crossings.append(n-1-samples[n-1]/(samples[n]-samples[n-1]))
            if len(crossings) < 10:
                rejected.append({'sample': start, 'reason': 'Insufficient pulse cycles'})
                continue
            frequency = rate / statistics.median(b-a for a,b in zip(crossings,crossings[1:]))
            pulse_id = round((frequency-997)/100)
            if pulse_id < 0 or abs(frequency-(997+100*pulse_id)) > 25:
                rejected.append({'sample': start, 'reason': f'Unrecognized pulse frequency {frequency:.2f} Hz'})
                continue
            pulses.append({'sample':start,'frequencyHz':frequency,'pulseId':pulse_id,'sourceFrame':pulse_id*60})
    return pulses, rejected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=pathlib.Path)
    parser.add_argument('--calibration', type=pathlib.Path)
    args = parser.parse_args()
    root = args.directory
    capture = json.loads((root/'av-capture.json').read_text())
    if not capture['ok']:
        raise ValueError('Capture failed; cannot establish alignment')
    audio = root/'audio.nut'
    info = json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','a:0',
        '-show_packets','-show_streams','-show_entries','stream=codec_name,sample_rate,channels:packet=pts_time,size',
        '-of','json',str(audio)],text=True))
    stream = info['streams'][0]
    if stream['codec_name'] != 'pcm_s16le' or stream['channels'] != 1:
        raise ValueError('Expected mono PCM16 sink capture')
    rate = int(stream['sample_rate'])
    raw = subprocess.check_output(['ffmpeg','-v','error','-i',str(audio),'-map','0:a:0','-c:a','copy','-f','s16le','pipe:1'])
    samples = array.array('h'); samples.frombytes(raw)
    if sys.byteorder != 'little': samples.byteswap()
    starts, times, cursor = [], [], 0
    for packet in info['packets']:
        starts.append(cursor); times.append(float(packet['pts_time'])*1000)
        cursor += int(packet['size'])//2
    if cursor != len(samples):
        raise ValueError('PCM and packet timestamp coverage differ')
    pulses, rejected = detect_pulses(samples, rate)
    markers = {}
    for frame in capture['capture']['records']:
        if frame.get('markerFrame') is None or frame.get('markerError'):
            continue
        if frame['callbackTime']-frame['expectedDisplayTime'] > 25:
            continue  # Do not assign a possibly newer image to a late callback.
        markers.setdefault(frame['markerFrame'],capture['capture']['timeOrigin']+frame['expectedDisplayTime'])
    correction = 0
    if args.calibration:
        calibration = json.loads(args.calibration.read_text())
        if not calibration['ok'] or calibration['captureMode'] != 'file':
            raise ValueError('A passing local-file calibration is required')
        for key in ('browser', 'browserVersion', 'executable'):
            if calibration.get(key) != capture.get(key):
                raise ValueError(f'Calibration uses a different {key}')
        correction = calibration['medianOffsetMs']
    def audio_epoch(sample):
        packet_index = bisect.bisect_right(starts, sample)-1
        return times[packet_index]+(sample-starts[packet_index])*1000/rate

    records = capture['capture']['records']
    window_start = capture['capture']['timeOrigin']+records[0]['expectedDisplayTime']
    window_end = capture['capture']['timeOrigin']+records[-1]['expectedDisplayTime']
    for item in rejected:
        item['audioEpochMs'] = audio_epoch(item['sample'])
        item['inMeasuredWindow'] = window_start <= item['audioEpochMs'] <= window_end
    matched = []
    for pulse in pulses:
        if pulse['sourceFrame'] not in markers:
            continue
        audio_time = audio_epoch(pulse['sample'])
        offset = markers[pulse['sourceFrame']]-audio_time
        matched.append({**pulse,'audioEpochMs':audio_time,'videoEpochMs':markers[pulse['sourceFrame']],
                        'offsetMs':offset,'calibratedOffsetMs':offset-correction})
    offsets = [m['offsetMs'] for m in matched]
    errors = [abs(m['calibratedOffsetMs']) for m in matched]
    result = {'captureMode':capture['mode'],
              **{key:capture.get(key) for key in ('browser','browserVersion','executable')},
              'matchedPulses':len(matched),'detectedPulses':len(pulses),
              'rejectedPulses':rejected,'minimumMatchedPulses':10,'maximumAllowedErrorMs':80,
              'calibrationOffsetMs':correction,'medianOffsetMs':statistics.median(offsets) if offsets else None,
              'maximumAbsoluteCalibratedErrorMs':max(errors) if errors else None,'matches':matched,
              'sign':'Positive means video follows audio at the virtual output sink.',
              'scope':capture['scope']}
    result['ok'] = len(matched)>=10 and max(errors,default=math.inf)<=80
    (root/'av-alignment.json').write_text(json.dumps(result,indent=2))
    print(json.dumps({k:v for k,v in result.items() if k!='matches'},indent=2))
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
