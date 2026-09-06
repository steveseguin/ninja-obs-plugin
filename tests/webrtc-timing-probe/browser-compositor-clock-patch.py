"""Add the actual compositor selection-call clock to the existing native trace."""
from pathlib import Path
import subprocess
import sys
root = Path(sys.argv[1]).resolve()
if subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip() != '47e20adcc15fc15f01825aa17e570c8f5492ac0f':
    raise RuntimeError('Expected pinned Chromium 145 source')
p = root / 'third_party/blink/renderer/modules/mediastream/web_media_player_ms_compositor.cc'
s = p.read_text()
a = '               << " deadline_us=" << deadline_min.ToInternalValue()'
b = '               << " now_us=" << base::TimeTicks::Now().ToInternalValue()\n' + a
if s.count(a) != 1 or b in s:
    raise RuntimeError('Expected one unpatched compositor selection site')
p.write_text(s.replace(a,b))
print('Applied actual selection-call clock trace')
