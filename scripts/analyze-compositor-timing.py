"""Join diagnostic decode release, compositor enqueue/selection and rVFC by RTP.

Native timestamps share Chromium's monotonic clock. Do not combine browser logs:
object pointers are only process-local, and headless logs may omit process IDs.
"""
import argparse
import json
import re
from collections import Counter
from pathlib import Path


def analyze(text, capture, fps):
    events = []
    for line in text.splitlines():
        if 'VDONINJA_COMPOSITOR ' in line or ('VDONINJA_TIMING ' in line and 'event=release ' in line):
            event = dict(re.findall(r'(\w+)=([^\s]+)', line))
            process = re.match(r'\[(\d+):', line)
            event['process'] = process.group(1) if process else 'unknown'
            for key in ['rtp', 'now_us', 'reference_us', 'media_us', 'deadline_us', 'deadline_max_us',
                        'dropped', 'repeated', 'now_ms', 'render_ms', 'receive_ms']:
                if key in event:
                    event[key] = int(float(event[key]))
            events.append(event)
    objects = {e['object'] for e in events if 'object' in e}
    timings = {e['timing'] for e in events if 'timing' in e}
    processes = {e['process'] for e in events if e['process'] != 'unknown'}
    if len(processes) > 1:
        raise ValueError('Do not combine different browser processes')
    if len(objects) != 1 or len(timings) != 1:
        raise ValueError('Require exactly one compositor and timing object in one browser log')
    enqueued = {e['rtp']: e for e in events if e.get('event') == 'enqueue'}
    released = {e['rtp']: e for e in events if e.get('event') == 'release'}
    selected = [e for e in events if e.get('event') == 'select' and not e['repeated']]
    ticks = 90000 / fps
    omissions = []
    for a, b in zip(selected, selected[1:]):
        delta = (b['rtp'] - a['rtp']) % 2**32
        frames = round(delta / ticks)
        if 1 < frames < fps * 10:
            missing = [int((a['rtp'] + i * ticks) % 2**32) for i in range(1, frames)]
            omissions.append({'beforeRtp': a['rtp'], 'afterRtp': b['rtp'], 'missingRtp': missing,
                              'missingReleased': [rtp in released for rtp in missing],
                              'missingEnqueued': [rtp in enqueued for rtp in missing],
                              'enqueueEvidence': [enqueued.get(rtp) for rtp in [a['rtp'], *missing, b['rtp']]],
                              'releaseEvidence': [released.get(rtp) for rtp in [a['rtp'], *missing, b['rtp']]],
                              'selection': b})
    cadence = []
    records = capture['records']
    measured_omissions = []
    if records:
        first_rtp, last_rtp = records[0]['rtpTimestamp'], records[-1]['rtpTimestamp']
        span = (last_rtp - first_rtp) % 2**32
        measured_omissions = [o for o in omissions
                              if (o['beforeRtp'] - first_rtp) % 2**32 <= span
                              and (o['afterRtp'] - first_rtp) % 2**32 <= span]
    for a, b in zip(records, records[1:]):
        count = b['presentedFrames'] - a['presentedFrames']
        interval = b['presentationTime'] - a['presentationTime']
        media = round(((b['rtpTimestamp'] - a['rtpTimestamp']) % 2**32) / ticks)
        if abs(interval / max(1, count) - 1000 / fps) > max(8, 750 / fps) or media > count:
            cadence.append({'beforeRtp': a['rtpTimestamp'], 'afterRtp': b['rtpTimestamp'],
                            'counterDelta': count, 'sourceFrameDelta': media, 'submissionIntervalMs': interval,
                            'displayIntervalMs': b['expectedDisplayTime'] - a['expectedDisplayTime'],
                            'callbackLatenessMs': b['callbackTime'] - b['expectedDisplayTime'],
                            'releaseBefore': released.get(a['rtpTimestamp']),
                            'releaseAfter': released.get(b['rtpTimestamp'])})
    return {'scope': 'Full native run plus separately identified measured rVFC window; no gates are waived.',
            'counts': dict(Counter(e.get('event') for e in events)),
            'nativeDropped': sum(e.get('dropped', 0) for e in events),
            'nativeOmissions': omissions, 'measuredNativeOmissions': measured_omissions,
            'measuredCadenceEvents': cadence}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('capture', type=Path)
    p.add_argument('--fps', type=float, default=60)
    a = p.parse_args()
    if not 0 < a.fps <= 240:
        p.error('FPS must be in (0, 240]')
    print(json.dumps(analyze(a.log.read_text(), json.loads(a.capture.read_text()), a.fps), indent=2))


if __name__ == '__main__':
    main()
