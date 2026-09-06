"""Compare native Firefox first-composition times with measured rVFC cadence.

RTP joins the page records to queued images; object/producer/frame identities join
those images to compositor notifications. Notification delivery time is not paint
time. This does not infer decoded pixels or physical display scanout.
"""
import argparse
import json
import re
import statistics
from pathlib import Path


def analyze(text, capture, fps=60):
    if not 0 < fps <= 240:
        raise ValueError('FPS must be in (0, 240]')
    queued, painted, processes = {}, {}, set()
    for line in text.splitlines():
        if 'VDONINJA_FF_COMPOSITOR ' not in line:
            continue
        fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
        process = re.search(r'\[(?:Child|Parent) (\d+):', line)
        if process:
            processes.add(process.group(1))
        for key in ['producer', 'frame', 'rtp', 'now_us', 'reference_us', 'composite_us', 'paint_count']:
            if key in fields:
                fields[key] = int(fields[key])
        identity = (fields['object'], fields['producer'], fields['frame'])
        if fields['event'] == 'enqueue':
            queued.setdefault(identity, fields)
        elif fields['event'] == 'composite':
            painted.setdefault(identity, fields)
    if len(processes) > 1:
        raise ValueError('Do not combine Firefox processes')
    records = capture['records']
    measured_rtp = {record['rtpTimestamp'] for record in records}
    streams = {(key[0], key[1]) for key, value in queued.items() if value['rtp'] in measured_rtp}
    if len(streams) != 1:
        raise ValueError('Require exactly one measured RTP-bearing image container/producer')
    stream = next(iter(streams))
    queued = {key: value for key, value in queued.items() if key[:2] == stream}
    by_rtp = {}
    for key, enqueue in queued.items():
        if enqueue['rtp'] in by_rtp and by_rtp[enqueue['rtp']]['enqueue']['frame'] != enqueue['frame']:
            raise ValueError('Ambiguous repeated RTP timestamp')
        by_rtp[enqueue['rtp']] = {'enqueue': enqueue, 'composite': painted.get(key)}
    matched = sum(bool(by_rtp.get(r['rtpTimestamp'], {}).get('composite')) for r in records)
    if not matched:
        raise ValueError('No measured RTP/composition matches')
    first_rtp = records[0]['rtpTimestamp']
    span = (records[-1]['rtpTimestamp'] - first_rtp) % 2**32
    native = sorted((value for rtp, value in by_rtp.items()
                     if value['composite'] and (rtp - first_rtp) % 2**32 <= span),
                    key=lambda value: value['composite']['composite_us'])
    omissions, native_intervals, non_forward = [], [], 0
    for before, after in zip(native, native[1:]):
        a, b = before['enqueue']['rtp'], after['enqueue']['rtp']
        delta = (b - a) % 2**32
        native_intervals.append((after['composite']['composite_us'] -
                                 before['composite']['composite_us']) / 1000)
        if delta == 0 or delta >= 2**31:
            non_forward += 1
            continue
        frames = round(delta / (90000 / fps))
        if frames > 1:
            missing = [round((a + i * 90000 / fps) % 2**32)
                       for i in range(1, min(frames, 601))]
            omissions.append({'beforeRtp': a, 'afterRtp': b, 'missingFrameCount': frames - 1,
                              'missingRtp': missing, 'missingQueued': [rtp in by_rtp for rtp in missing],
                              'evidenceTruncated': frames > 601})
    failures = []
    for before, after in zip(records, records[1:]):
        count = after['presentedFrames'] - before['presentedFrames']
        interval = after['presentationTime'] - before['presentationTime']
        source = round(((after['rtpTimestamp'] - before['rtpTimestamp']) % 2**32) / (90000 / fps))
        if abs(interval / max(1, count) - 1000 / fps) <= max(8, 750 / fps) and source <= count:
            continue
        a, b = by_rtp.get(before['rtpTimestamp']), by_rtp.get(after['rtpTimestamp'])
        native_interval = None
        if a and b and a['composite'] and b['composite']:
            native_interval = (b['composite']['composite_us'] - a['composite']['composite_us']) / 1000
        failures.append({'beforeRtp': before['rtpTimestamp'], 'afterRtp': after['rtpTimestamp'],
                         'counterDelta': count, 'sourceFrameDelta': source,
                         'submissionIntervalMs': interval, 'nativeCompositionIntervalMs': native_interval,
                         'before': a, 'after': b})
    native_missing = sum(item['missingFrameCount'] for item in omissions)
    native_duration = sum(native_intervals)
    average_ratio = (len(native_intervals) * 1000 / native_duration / fps) if native_duration > 0 else 0
    max_deviation = max((abs(interval - 1000 / fps) for interval in native_intervals), default=0)
    native_failures = []
    if matched != len(records):
        native_failures.append(f'{len(records) - matched} measured callback frames lack composition notifications')
    if native_missing or non_forward:
        native_failures.append(f'{native_missing} native composition omissions, {non_forward} non-forward RTP transitions')
    if average_ratio < .95:
        native_failures.append('Native composition average FPS ratio is below 0.95')
    if max_deviation > max(8, 750 / fps):
        native_failures.append(f'Native composition cadence deviation {max_deviation:.3f} ms exceeds {max(8, 750 / fps)} ms')
    if max(native_intervals, default=0) > 100:
        native_failures.append('Native composition gap exceeds 100 ms')
    return {'scope': __doc__, 'ok': not native_failures, 'failures': native_failures,
            'queuedFrames': len(queued), 'compositedRtpFrames':
            sum(bool(value['composite']) for value in by_rtp.values()),
            'measuredRecords': len(records), 'matchedMeasuredRecords': matched,
            'unmatchedMeasuredRecords': len(records) - matched, 'measuredCadenceEvents': failures,
            'nativeMeasuredFrames': len(native), 'nativeNonForwardRtp': non_forward,
            'nativeMedianCompositionIntervalMs': statistics.median(native_intervals) if native_intervals else None,
            'nativeMaximumCompositionIntervalMs': max(native_intervals) if native_intervals else None,
            'nativeAverageFpsRatio': average_ratio, 'nativeMaximumCadenceDeviationMs': max_deviation,
            'nativeOmittedFrames': native_missing,
            'nativeOmissions': omissions}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('capture', type=Path, help='Native driver round JSON or presentation capture JSON')
    p.add_argument('--fps', type=float, default=60)
    a = p.parse_args()
    if not 0 < a.fps <= 240:
        p.error('FPS must be in (0, 240]')
    capture = json.loads(a.capture.read_text())
    print(json.dumps(analyze(a.log.read_text(), capture.get('capture', capture), a.fps), indent=2))


if __name__ == '__main__':
    main()
