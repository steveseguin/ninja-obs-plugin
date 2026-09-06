"""Add a small diagnostic executable to the pinned Chromium source build."""
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
if subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() != '47e20adcc15fc15f01825aa17e570c8f5492ac0f':
    raise RuntimeError('Expected Chromium 145.0.7632.6')
folder = root / 'vdoninja_receiver_probe'
if folder.exists():
    raise RuntimeError('Probe directory already exists; inspect it before replacing')
build = root / 'BUILD.gn'
text = build.read_text()
if 'vdoninja_renderer_probe' in text:
    raise RuntimeError('Probe root group already exists')
folder.mkdir()
shutil.copyfile(Path(__file__).with_name('renderer-probe.cpp'), folder / 'renderer-probe.cpp')
shutil.copyfile(Path(__file__).with_name('renderer.BUILD.gn'), folder / 'BUILD.gn')
build.write_text(text + '\n# Local receiver timing diagnostic; never shipped.\ngroup("vdoninja_renderer_probe") {\n  deps = [ "//vdoninja_receiver_probe:renderer_probe" ]\n}\n')
print('Prepared renderer_probe GN target')
