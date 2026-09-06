#!/usr/bin/env python3
"""Summarize opt-in browser receiver timing logs without inferring missing state."""
import argparse
import json
import re
import statistics
from collections import defaultdict


def analyze(lines):
    objects = defaultdict(lambda: {
        'insertions': 0, 'nackedInsertions': 0, 'incomingSamples': 0,
        'uninitializedQueries': 0, 'bufferedUninitializedQueries': 0,
        'renderQueries': 0, 'renderLeadsMs': [], 'requestedMinimumsMs': set(),
        'releasedFrames': 0, 'releaseLeadsMs': [],
    })
    malformed = 0
    for line in lines:
        if 'VDONINJA_TIMING ' not in line:
            continue
        fields = dict(re.findall(r'(\w+)=([^\s]+)', line.split('VDONINJA_TIMING ', 1)[1]))
        event = fields.get('event')
        pid = re.match(r'\[(\d+):', line) or re.search(r'\[(?:Child|Parent) (\d+)[,:]', line)
        try:
            timing = fields['timing']
            if event not in ('insert', 'incoming', 'uninitialized', 'render', 'release'):
                raise ValueError('Unknown event')
            # Parse before accumulating so malformed records cannot partly count.
            nack = int(fields['nack']) if event == 'insert' else None
            minimum = int(fields['min_ms']) if event in ('uninitialized', 'render') else None
            lead = int(fields['render_ms']) - int(fields['now_ms']) if event in ('render', 'release') else None
        except (KeyError, ValueError):
            malformed += 1
            continue
        obj = objects[(pid.group(1) if pid else 'unknown', timing)]
        if minimum is not None:
            obj['requestedMinimumsMs'].add(minimum)
        if event == 'insert':
            obj['insertions'] += 1
            obj['nackedInsertions'] += bool(nack)
        elif event == 'incoming':
            obj['incomingSamples'] += 1
        elif event == 'uninitialized':
            obj['uninitializedQueries'] += 1
            obj['bufferedUninitializedQueries'] += minimum > 0
        elif event == 'render':
            obj['renderQueries'] += 1
            obj['renderLeadsMs'].append(lead)
        elif event == 'release':
            obj['releasedFrames'] += 1
            obj['releaseLeadsMs'].append(lead)
    result = []
    for (pid, timing), obj in objects.items():
        leads = obj.pop('renderLeadsMs')
        releases = obj.pop('releaseLeadsMs')
        obj['requestedMinimumsMs'] = sorted(obj['requestedMinimumsMs'])
        result.append({'pid': pid, 'timing': timing, **obj,
                       'minimumReleaseLeadMs': min(releases) if releases else None,
                       'medianReleaseLeadMs': statistics.median(releases) if releases else None,
                       'minimumRenderLeadMs': min(leads) if leads else None,
                       'medianRenderLeadMs': statistics.median(leads) if leads else None,
                       'maximumRenderLeadMs': max(leads) if leads else None})
    return {'objects': result, 'malformedRecords': malformed,
            'coverage': 'Logged queries only; missing events are not inferred.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log')
    args = parser.parse_args()
    with open(args.log, encoding='utf-8', errors='replace') as source:
        result = analyze(source)
    print(json.dumps(result, indent=2))
    return 0 if result['objects'] and not result['malformedRecords'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
