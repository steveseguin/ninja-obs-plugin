"""Join Chromium native A/V sync decisions, buffer writes and measured cadence.

The clock offset comes from the same encoded frame's native ReceivedTime and
JavaScript receiveTime, not from wall-clock assumptions. Nearby events are
correlation evidence; this analyzer does not assign causation or waive gates.
"""
import argparse
import json
import re
import runpy
import statistics
from pathlib import Path

compositor_analyze = runpy.run_path(str(Path(__file__).with_name('analyze-compositor-timing.py')))['analyze']


def analyze(text, encoded, capture, fps=60):
    frames = {int(x['rtpTimestamp']): x for x in encoded.get('records', [])
              if x.get('metadata', {}).get('mimeType', '').lower() == 'video/h264'}
    offsets, sync, renders = [], [], {}
    for line in text.splitlines():
        if not any(tag in line for tag in ['VDONINJA_TIMING ', 'VDONINJA_SYNC ']):
            continue
        fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
        for key in ['rtp', 'receive_ms', 'now_ms', 'render_ms', 'min_ms', 'audio_ssrc', 'video_ssrc',
                    'relative_ms', 'current_audio_ms', 'current_video_ms', 'target_audio_ms',
                    'target_video_ms', 'adjusted']:
            if key in fields:
                fields[key] = float(fields[key])
        if 'VDONINJA_SYNC ' in line:
            sync.append(fields)
        elif fields.get('event') == 'release':
            frame = frames.get(fields['rtp'])
            receive = frame.get('metadata', {}).get('receiveTime') if frame else None
            if receive is not None and 'receive_ms' in fields:
                offsets.append(fields['receive_ms'] - receive)
        elif fields.get('event') == 'render':
            renders[fields['rtp']] = fields
    if not offsets:
        raise ValueError('No matching encoded/native receive-time clock anchors')
    offset = statistics.median(offsets)
    spread = max(offsets) - min(offsets)
    if spread > 1:
        raise ValueError('Receive-time clock anchors differ by more than 1 ms')
    if not sync or len({s['object'] for s in sync}) != 1:
        raise ValueError('Require exactly one native A/V synchronizer')
    video_ssrc = {x['metadata'].get('synchronizationSource') for x in frames.values()}
    if len(video_ssrc) != 1 or {s['video_ssrc'] for s in sync} != video_ssrc:
        raise ValueError('Native synchronizer and encoded video SSRC do not match')
    writes = [dict(w, nativeMs=w['now'] + offset) for w in encoded.get('bufferWrites', [])]
    joined = compositor_analyze(text, capture, fps)
    for event in joined['measuredCadenceEvents']:
        after = event.get('releaseAfter')
        if not after:
            continue
        now = after['now_ms']
        event['renderBefore'] = renders.get(event['beforeRtp'])
        event['renderAfter'] = renders.get(event['afterRtp'])
        event['nearbySyncDecisions'] = [s for s in sync if now - 2000 <= s['now_ms'] <= now + 50]
        event['nearbyBufferWrites'] = [w for w in writes if now - 3000 <= w['nativeMs'] <= now + 50]
    return {'scope': __doc__, 'clock': {'anchorCount': len(offsets), 'nativeMinusBrowserMs': offset,
                                       'anchorSpreadMs': spread},
            'bufferCapabilities': encoded.get('capabilities'), 'bufferWrites': writes,
            'syncDecisions': sync, 'adjustmentCount': sum(bool(s['adjusted']) for s in sync),
            'compositor': joined}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('encoded', type=Path)
    p.add_argument('capture', type=Path)
    p.add_argument('--fps', type=float, default=60)
    a = p.parse_args()
    if not 0 < a.fps <= 240:
        p.error('FPS must be in (0, 240]')
    print(json.dumps(analyze(a.log.read_text(), json.loads(a.encoded.read_text()),
                             json.loads(a.capture.read_text()), a.fps), indent=2))


if __name__ == '__main__':
    main()
