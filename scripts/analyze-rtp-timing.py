#!/usr/bin/env python3
"""Correlate synthetic RTP headers, sender reports, encoded-frame arrivals and composition."""
import argparse
import csv
import importlib.util
import json
from pathlib import Path
import statistics

spec = importlib.util.spec_from_file_location('rtp_pcap', Path(__file__).resolve().parents[1] / 'tests/tools/rtp_pcap.py')
pcap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pcap)


def distribution(values):
    if not values:
        return None
    values = sorted(values)
    return {'count': len(values), 'min': values[0], 'median': statistics.median(values),
            'p95': values[min(len(values)-1, int(len(values)*.95))], 'max': values[-1]}


def nack_coverage(rows, received_timestamps):
    current_sequence_frame = {}
    nacked = set()
    for row in sorted(rows, key=lambda r: int(r['steady_ns'])):
        kind = int(row['kind'])
        sequence = int(row['sequence'])
        if kind == 96:
            current_sequence_frame[sequence] = int(row['rtp_timestamp'])
        elif kind == 205 and sequence in current_sequence_frame:
            nacked.add(current_sequence_frame[sequence])
    received = list(dict.fromkeys(received_timestamps))
    clean = [timestamp for timestamp in received if timestamp not in nacked]
    return {'receivedFrames':len(received), 'receivedFramesWithNack':len(received)-len(clean),
            'receivedFramesWithoutNack':len(clean), 'firstCleanTimestamps':clean[:10],
            'allReceivedFramesNacked':bool(received) and not clean}


def read_native_trace(file):
    lines = Path(file).read_text().splitlines()
    if not lines or lines[-1] != '# overflow=0':
        raise ValueError(f'Incomplete or overflowed timing trace: {file}')
    rows = list(csv.DictReader(line for line in lines if not line.startswith('#')))
    return rows, '# version=2 nack_capture=1' in lines


def analyze(directory, relay='172.17.0.2'):
    directory = Path(directory)
    timing_files = list(directory.glob('obs-encoded-timing-*.json')) or list(directory.glob('viewer-encoded.json'))
    capture_files = list(directory.glob('obs-presentation-records-*.json')) or list(directory.glob('presentation.json'))
    timing = json.loads(timing_files[0].read_text())
    capture = json.loads(capture_files[0].read_text())
    if timing.get('overflow') or timing.get('error'):
        raise ValueError('Encoded timing trace overflowed or failed')
    encoded = [r for r in timing['records'] if r.get('metadata', {}).get('mimeType', '').startswith('video/')]
    if not encoded:
        raise ValueError('No encoded video trace; this browser may not expose the probe')
    ssrc = encoded[0]['metadata']['synchronizationSource']
    frames = {}
    for r in encoded:
        frame = frames.setdefault(r['rtpTimestamp'], {'rtpTimestamp': r['rtpTimestamp']})
        frame['encodedReady'] = r['epoch']
        frame['encodedBytes'] = r['bytes']
    for r in capture['records']:
        frame = frames.setdefault(r['rtpTimestamp'], {'rtpTimestamp': r['rtpTimestamp']})
        frame['composition'] = timing['timeOrigin'] + r['presentationTime']
        for k in ['receiveTime', 'captureTime']:
            if r.get(k) is not None:
                frame[k] = timing['timeOrigin'] + r[k]
    packets = {}
    for r in pcap.read_pcap(directory / 'network.pcap'):
        if r['ssrc'] != ssrc:
            continue
        key = (r['rtpTimestamp'], r['sequence'])
        entry = packets.setdefault(key, {})
        direction = 'receive' if r['src'] == relay else 'send'
        entry[direction] = min(r['epochMs'], entry.get(direction, float('inf')))
    transport_delays = []
    for (timestamp, sequence), packet in packets.items():
        frame = frames.setdefault(timestamp, {'rtpTimestamp': timestamp})
        for direction, time in packet.items():
            frame[f'{direction}First'] = min(time, frame.get(f'{direction}First', float('inf')))
            frame[f'{direction}Last'] = max(time, frame.get(f'{direction}Last', float('-inf')))
        if 'send' in packet and 'receive' in packet:
            transport_delays.append(packet['receive']-packet['send'])
    reports = []
    native_rows = []
    nack_available = False
    for file in directory.glob('rtp-*.csv'):
        rows, has_nack_capture = read_native_trace(file)
        matching = [r for r in rows if int(r['ssrc']) == ssrc]
        if matching:
            nack_available = has_nack_capture if not native_rows else nack_available and has_nack_capture
            native_rows.extend(matching)
        for r in rows:
            if int(r['ssrc']) == ssrc and int(r['kind']) == 200:
                reports.append({'unixMs': int(r['unix_ns'])/1e6, 'rtpTimestamp': int(r['rtp_timestamp']),
                                'ntpMs': (int(r['ntp_seconds'])-2208988800)*1000 + int(r['ntp_fraction'])*1000/(2**32)})
    reports.sort(key=lambda r:r['unixMs'])
    if reports:
        first = reports[0]
        for r in reports:
            r['mappingDriftMs'] = r['ntpMs']-first['ntpMs']-((r['rtpTimestamp']-first['rtpTimestamp']) & 0xffffffff)/90
    # A gap with only one newly composed frame identifies the omitted source RTP frames.
    # Counter jumps >1 mean missed callbacks; those intervals cannot identify individual drops.
    omissions = []
    for before, after in zip(capture['records'], capture['records'][1:]):
        ticks = (after['rtpTimestamp']-before['rtpTimestamp']) & 0xffffffff
        if after['presentedFrames']-before['presentedFrames'] != 1 or not 4500 <= ticks < 90000:
            continue
        for rtp, frame in frames.items():
            delta = (rtp-before['rtpTimestamp']) & 0xffffffff
            if 0 < delta < ticks and 'encodedReady' in frame:
                omissions.append({**frame, 'nextComposition': timing['timeOrigin']+after['presentationTime'],
                                  'readyBeforeNextCompositionMs': timing['timeOrigin']+after['presentationTime']-frame['encodedReady']})
    summary = {'ssrc': ssrc, 'nackCoverage': nack_coverage(native_rows, [r['rtpTimestamp'] for r in encoded]) if native_rows and nack_available else None, 'encodedFrames': len(encoded), 'compositionCallbacks': len(capture['records']),
               'matchedPacketTransitMs': distribution(transport_delays),
               'senderFrameSpreadMs': distribution([r['sendLast']-r['sendFirst'] for r in frames.values() if 'sendLast' in r]),
               'encodedReadyToCompositionMs': distribution([r['composition']-r['encodedReady'] for r in frames.values() if 'composition' in r and 'encodedReady' in r]),
               'senderReportDriftMs': distribution([r['mappingDriftMs'] for r in reports]),
               'definiteCompositionOmissions': omissions}
    (directory / 'rtp-frame-timeline.json').write_text(json.dumps({'frames':list(frames.values()), 'senderReports':reports},indent=2))
    (directory / 'timing-analysis.json').write_text(json.dumps(summary,indent=2))
    print(json.dumps({k:v if k != 'definiteCompositionOmissions' else len(v) for k,v in summary.items()},indent=2))
    return summary

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory')
    parser.add_argument('--relay', default='172.17.0.2')
    args = parser.parse_args()
    analyze(args.directory,args.relay)
