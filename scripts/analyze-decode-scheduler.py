"""Join native metronome deadlines/releases to Chromium compositor failures.

This is diagnostic evidence, not a replacement for media gates. Native scheduler
and renderer timestamps use the browser monotonic clock; RTP joins frames.
"""
import argparse
import json
import re
import runpy
import statistics
from pathlib import Path


def parse(text):
    pending, released, selections = {}, {}, []
    for line in text.splitlines():
        if 'VDONINJA_SCHED ' not in line:
            continue
        event = dict(re.findall(r'(\w+)=([^\s]+)', line))
        for key in ['rtp', 'ssrc', 'metronome', 'now_us', 'deadline_us', 'render_us',
                    'next_tick_us', 'period_us', 'immediate', 'release']:
            if key in event:
                event[key] = int(event[key])
        if event['event'] == 'select':
            selections.append(event)
            continue
        key = (event['scheduler'], event['rtp'])
        if event['event'] == 'schedule':
            pending[key] = {'schedule': event, 'decisions': []}
        elif event['event'] in ['decision', 'tick']:
            if key in pending:
                pending[key]['decisions'].append(event)
        elif event['event'] == 'release':
            if key not in pending:
                raise ValueError('Release lacks a matching scheduler/RTP schedule')
            frame = pending.pop(key)
            if event['rtp'] in released:
                raise ValueError('Ambiguous repeated released RTP; separate streams/processes')
            if event['deadline_us'] != frame['schedule']['deadline_us']:
                raise ValueError('Schedule/release deadline mismatch')
            frame.update(release=event,
                         releaseMinusDeadlineMs=(event['now_us'] - event['deadline_us']) / 1000,
                         renderLeadMs=(frame['schedule']['render_us'] - event['now_us']) / 1000)
            released[event['rtp']] = frame
    if not released:
        raise ValueError('No matched native scheduler releases')
    return selections, released


def analyze(text, capture, fps=60):
    selections, frames = parse(text)
    measured = {r['rtpTimestamp'] for r in capture['records']}
    selected = [frame for rtp, frame in frames.items() if rtp in measured]
    if not selected:
        raise ValueError('No measured RTP matches native scheduler releases')
    values = [f['releaseMinusDeadlineMs'] for f in selected]
    enqueued, renders, selections_nearby = {}, {}, []
    for line in text.splitlines():
        if not ('VDONINJA_COMPOSITOR ' in line or ('VDONINJA_TIMING ' in line and 'event=render ' in line)):
            continue
        event = dict(re.findall(r'(\w+)=([^\s]+)', line))
        for key in ['rtp', 'now_us', 'reference_us', 'deadline_us', 'deadline_max_us',
                    'repeated', 'dropped', 'now_ms', 'render_ms', 'min_ms', 'decode_ms',
                    'render_delay_ms', 'current_delay_ms', 'headroom_ms']:
            if key in event:
                event[key] = float(event[key])
        if event['event'] == 'enqueue':
            enqueued[event['rtp']] = event
        elif event['event'] == 'render':
            renders[event['rtp']] = event
        elif event['event'] == 'select':
            selections_nearby.append(event)
    compositor = runpy.run_path(str(Path(__file__).with_name('analyze-compositor-timing.py')))['analyze'](text, capture, fps)
    for event in compositor['measuredCadenceEvents']:
        event['schedulerBefore'] = frames.get(event['beforeRtp'])
        event['schedulerAfter'] = frames.get(event['afterRtp'])
        event['enqueueBefore'] = enqueued.get(event['beforeRtp'])
        event['enqueueAfter'] = enqueued.get(event['afterRtp'])
        event['renderBefore'] = renders.get(event['beforeRtp'])
        event['renderAfter'] = renders.get(event['afterRtp'])
        after = event['enqueueAfter']
        if after:
            reference = after['reference_us']
            event['nearbySelections'] = [s for s in selections_nearby
                                        if reference - 100000 <= s['deadline_us'] <= reference + 50000]
    return {'scope': __doc__, 'selections': selections, 'matchedMeasuredFrames': len(selected),
            'unmatchedMeasuredFrames': len(measured) - len(selected),
            'releaseMinusDeadlineMs': {'minimum': min(values), 'median': statistics.median(values),
                                       'maximum': max(values)},
            'compositor': compositor}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('capture', type=Path)
    p.add_argument('--fps', type=float, default=60)
    a = p.parse_args()
    if not 0 < a.fps <= 240:
        p.error('FPS must be in (0, 240]')
    capture = json.loads(a.capture.read_text())
    print(json.dumps(analyze(a.log.read_text(), capture.get('capture', capture), a.fps), indent=2))


if __name__ == '__main__':
    main()
