"""Check the recorded room observations and the real local RTMP recording."""
import json
from pathlib import Path
import subprocess
import sys
from urllib.parse import parse_qs, urlparse

import numpy as np

output = Path(sys.argv[1]).resolve()
state = json.loads((output / 'state.json').read_text())
room = json.loads((output / 'room-results.json').read_text())
summary = {'guests': 3, 'director': 1, 'video': '1280x720', 'checks': {}, 'audio': {}}
expected = {g['name']: g for g in room['guests']}
assert len(expected) == 3
for phase in ['public-stream-active', 'reconnected', 'public-stopped-return-active',
              'custom-signaling-restart-settled', 'final-continuity']:
    for guest in room['checks'][phase][:3]:
        assert guest['stream'] == expected[guest['name']]['stream'], 'Guest identity changed on reconnect'
        program = next(p for p in guest['peers'] if p.get('stream') == state['stream'])
        assert program['state'] == 'connected' and program['width'] == 1280 and program['height'] == 720
        video = next(p for p in program['rtp'] if p['kind'] == 'video')
        audio = next(p for p in program['rtp'] if p['kind'] == 'audio')
        assert video['frames'] > 20 and audio['bytes'] > 10000
        levels = program['tones']
        wanted = min(levels['1200'], levels['1500'])
        unwanted = max(levels[str(hz)] for hz in [300,600,900])
        assert wanted > -65 and wanted-unwanted > 45, (phase, guest['name'], levels)
        for other in expected.values():
            if other['name'] == guest['name']: continue
            peer = next(p for p in guest['peers'] if p.get('stream') == other['stream'])
            assert peer['width'] == 0 and peer['state'] == 'connected'
            assert any(p['kind']=='audio' and p['bytes']>1000 for p in peer['rtp'])
            assert not any(p['kind']=='video' and p.get('frames',0)>0 for p in peer['rtp'])
            assert peer['tones'][str(other['frequency'])] > -85
        summary['audio'][phase+'/'+guest['name']] = {'returnGuestRejectionDb':round(wanted-unwanted,1)}
    summary['checks'][phase] = True

for before, after in zip(room['checks']['custom-signaling-restart-settled'][:3],room['checks']['final-continuity'][:3]):
    def frames(guest):
        program=next(p for p in guest['peers'] if p.get('stream')==state['stream'])
        return next(p['frames'] for p in program['rtp'] if p['kind']=='video')
    assert frames(after)>frames(before)+20
summary['checks']['videoContinuesForAllGuests'] = True
extra = room['checks']['capacity-rejection'][3]
assert not any(p.get('stream')==state['stream'] and p.get('width',0) for p in extra['peers'])
logs='\n'.join(p.read_text(encoding='utf-8',errors='replace') for p in (output/'obs/config/obs-studio/logs').glob('*.txt'))
assert 'max viewers reached (4)' in logs
summary['checks']['capacityRejectsFifthViewer'] = True
assert json.loads((output/'main-survived-return-stop.json').read_text(encoding='utf-8-sig'))['outputActive']
summary['checks']['mainSurvivesReturnStop'] = True
saved=json.loads((output/'persisted-settings.json').read_text())
assert saved['MaxViewers']=='6' and saved['CustomICEServers']=='stun:stun.l.google.com:19302'
assert saved['SignalingServer']=='wss://proxywss.rtc.ninja:443' and saved['ForceTURN']=='false'
summary['checks']['settingsPersistAcrossObsRestart'] = True
for filename in ['custom-invite.txt','custom-viewer.txt']:
    url=(output/filename).read_text()
    assert 'demo-user' not in url and 'demo-secret' not in url and 'relay.example.test' not in url
    query=parse_qs(urlparse(url).query)
    assert query['wss2']==['wss://proxywss.rtc.ninja:443']
invite=parse_qs(urlparse((output/'invite.txt').read_text()).query)
assert invite['broadcast']==[state['stream']] and invite['room']==[state['room']]
assert 'push' not in invite and 'view' not in invite and 'solo' not in invite
summary['checks']['copiedLinks'] = True
click = json.loads((output/'audio-click-verified.json').read_text())
assert click['ok'] and click['includedDb']-click['excludedDb']>40
summary['checks']['actualObsTrackCheckboxControlsReturnedGuestAudio'] = True
summary['audioCheckboxDbDifference'] = round(click['differenceDb'],1)

ffmpeg=Path(__file__).resolve().parent.parent/'build-plugin-release-validation/deps/obs-studio/.deps/obs-deps-2026-07-15-x64/bin/ffmpeg.exe'
decoded=subprocess.run([str(ffmpeg),'-v','error','-ss','5','-i',str(output/'public-stream.mkv'),'-t','2',
    '-vn','-ac','1','-ar','48000','-f','f32le','pipe:1'],capture_output=True,check=True)
samples=np.frombuffer(decoded.stdout,dtype='<f4');assert len(samples)>48000
fft=np.abs(np.fft.rfft(samples*np.hanning(len(samples))))*2/len(samples)
levels={}
for frequency in [300,600,900,1200,1500]:
    center=round(frequency*len(samples)/48000)
    levels[frequency]=round(float(20*np.log10(max(1e-12,max(fft[center-2:center+3])))),1)
    assert levels[frequency]>-80, levels
summary['publicAudioDbfs']=levels
summary['checks']['publicMixContainsEveryGuestHostAndClip'] = True
summary['ok']=True
(output/'verified-results.json').write_text(json.dumps(summary,indent=2))
print(json.dumps(summary,indent=2))
