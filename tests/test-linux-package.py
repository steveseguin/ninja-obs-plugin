#!/usr/bin/env python3
"""Run unmodified package scripts in a disposable chroot; invoke with sudo python3.

The fixtures are real ELF modules with a private shared dependency. OBS is a
version-reporting fixture; actual OBS-linked loading is a separate release gate.
"""
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]


class LinuxPackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.geteuid() != 0:
            raise RuntimeError('Run with sudo python3 tests/test-linux-package.py (isolated chroot tests)')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ninja-package-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.root.chmod(0o755)
        (self.root / 'dev').mkdir()
        os.mknod(self.root / 'dev/null', stat.S_IFCHR | 0o666, os.makedev(1, 3))
        (self.root / 'dev/null').chmod(0o666)
        for name in ('bash', 'dirname', 'mkdir', 'cp', 'rm', 'readlink', 'ldd'):
            binary = Path(shutil.which(name))
            self.copy(binary)
            info = subprocess.run(['ldd', str(binary)], text=True, capture_output=True)
            for path in re.findall(r'(/[^\s()]+)', info.stdout):
                self.copy(Path(path))
        # /bin/bash is the interpreter used by Ubuntu's ldd script.
        (self.root / 'bin').symlink_to('usr/bin')
        self.pkg = self.root / 'package'
        self.pkg.mkdir()
        for src, dst in [('install-package-linux.sh', 'install.sh'),
                         ('uninstall-package-linux.sh', 'uninstall.sh')]:
            shutil.copy2(REPO / 'scripts' / src, self.pkg / dst)
        self.data = self.pkg / 'share/obs/obs-plugins/obs-vdoninja'
        self.data.mkdir(parents=True)
        (self.data / 'sentinel.txt').write_text('locale data')
        self.obs_version('OBS Studio - 32.2.0')

    def copy(self, source):
        dest = self.root / str(source).lstrip('/')
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)

    def obs_version(self, text):
        obs = self.root / 'usr/bin/obs'
        obs.write_text('#!/usr/bin/bash\necho "' + text + '"\n')
        obs.chmod(0o755)

    def fixture(self, layout='lib/x86_64-linux-gnu/obs-plugins'):
        self.plugin_dir = self.pkg / layout
        runtime = self.plugin_dir / 'obs-vdoninja'
        runtime.mkdir(parents=True)
        subprocess.run(['cc', '-shared', '-fPIC', '-x', 'c', '-', '-o',
                        str(runtime / 'libdatachannel.so.0.20'),
                        '-Wl,-soname,libdatachannel.so.0.20'],
                       input='int dependency(void) { return 1; }', text=True, check=True)
        self.plugin = self.plugin_dir / 'obs-vdoninja.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-x', 'c', '-', '-o', str(self.plugin),
                        '-L' + str(runtime), '-l:libdatachannel.so.0.20',
                        '-Wl,-rpath,$ORIGIN/obs-vdoninja'],
                       input='extern int dependency(void); int use(void) { return dependency(); }\n'
                             'unsigned obs_module_ver(void) { return 0x20020000; }',
                       text=True, check=True)

    def run_script(self, script, *args, user=False, success=True, extra_env=None):
        command = ['chroot']
        if user:
            command += ['--userspec=1000:1000']
        command += [str(self.root), '/usr/bin/bash', '/package/' + script, *args]
        env = {**os.environ, 'LC_ALL': 'C'}
        env.pop('XDG_CONFIG_HOME', None)
        env.update(extra_env or {})
        result = subprocess.run(command, text=True, capture_output=True, env=env)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_system_layouts_round_trip(self):
        for layout in ('lib/obs-plugins', 'lib64/obs-plugins', 'lib/x86_64-linux-gnu/obs-plugins',
                       'lib64/test-triplet/obs-plugins', 'obs-plugins/64bit'):
            with self.subTest(layout=layout):
                self.fixture(layout)
                dst = self.root / 'usr' / (layout if layout.startswith('lib') else 'lib/obs-plugins')
                dst.mkdir(parents=True, exist_ok=True)
                (dst / 'unrelated.so').write_text('keep')
                self.run_script('install.sh')
                self.assertTrue((dst / 'obs-vdoninja.so').exists())
                self.assertTrue((dst / 'obs-vdoninja/libdatachannel.so.0.20').exists())
                self.run_script('uninstall.sh')
                self.assertFalse((dst / 'obs-vdoninja.so').exists())
                self.assertFalse((dst / 'obs-vdoninja').exists())
                self.assertTrue((dst / 'unrelated.so').exists())
                data = self.root / 'usr/share/obs/obs-plugins/obs-vdoninja'
                self.assertTrue((data / 'sentinel.txt').exists())
                self.run_script('uninstall.sh', '--remove-data')
                self.assertFalse(data.exists())
                shutil.rmtree(dst)
                shutil.rmtree(self.plugin_dir)

    def test_uninstall_cleans_duplicate_and_legacy_names(self):
        for layout in ('lib/obs-plugins', 'lib64/obs-plugins', 'lib/x86_64-linux-gnu/obs-plugins'):
            dst = self.root / 'usr' / layout
            dst.mkdir(parents=True)
            for name in ('obs-vdoninja.so', 'libobs-vdoninja.so', 'unrelated.so'):
                (dst / name).touch()
        self.run_script('uninstall.sh')
        self.assertEqual(len(list((self.root / 'usr').rglob('unrelated.so'))), 3)
        self.assertFalse(list((self.root / 'usr').rglob('*obs-vdoninja.so')))
        self.run_script('uninstall.sh')  # idempotent

    def test_multiarch_preferred_over_stale_plain_directory(self):
        self.fixture()
        plain = self.root / 'usr/lib/obs-plugins'
        multiarch = self.root / 'usr/lib/x86_64-linux-gnu/obs-plugins'
        plain.mkdir(parents=True)
        multiarch.mkdir(parents=True)
        self.run_script('install.sh')
        self.assertTrue((multiarch / 'obs-vdoninja.so').exists())
        self.assertFalse((plain / 'obs-vdoninja.so').exists())

    def test_selected_obs_library_controls_system_prefix(self):
        self.fixture()
        for prefix in ('usr', 'usr/local'):
            (self.root / prefix / 'lib/x86_64-linux-gnu/obs-plugins').mkdir(parents=True)
        for prefix in ('usr/local', 'usr'):
            with self.subTest(prefix=prefix):
                libdir = self.root / prefix / 'lib/x86_64-linux-gnu'
                subprocess.run(['cc', '-shared', '-fPIC', '-x', 'c', '-', '-o',
                                str(libdir / 'libobs.so.30'), '-Wl,-soname,libobs.so.30'],
                               input='int obs_fixture(void) { return 0; }', text=True, check=True)
                subprocess.run(['cc', '-x', 'c', '-', '-o', str(self.root / 'usr/bin/obs'),
                                '-L' + str(libdir), '-l:libobs.so.30',
                                '-Wl,-rpath,/' + prefix + '/lib/x86_64-linux-gnu'],
                               input='#include <stdio.h>\nextern int obs_fixture(void);\n'
                                     'int main(void) { puts("OBS Studio - 32.2.2"); return obs_fixture(); }',
                               text=True, check=True)
                self.run_script('install.sh')
                self.assertTrue((libdir / 'obs-plugins/obs-vdoninja.so').exists())
                data = self.root / prefix / 'share/obs/obs-plugins/obs-vdoninja'
                self.assertTrue((data / 'sentinel.txt').exists())
                self.run_script('uninstall.sh', '--remove-data')
                self.assertFalse((libdir / 'obs-plugins/obs-vdoninja.so').exists())
                self.assertFalse((libdir / 'obs-plugins/obs-vdoninja').exists())
                self.assertFalse(data.exists())

    def test_linux_and_release_obs_baselines_match(self):
        release = (REPO / '.github/workflows/build.yml').read_text()
        linux = (REPO / '.github/workflows/linux.yml').read_text()
        for key in ('OBS_VERSION', 'OBS_SOURCE_REF', 'OBS_SOURCE_TAG', 'LIBDATACHANNEL_VERSION'):
            pattern = r'^  ' + key + r': (\S+)$'
            expected = re.search(pattern, release, re.MULTILINE)
            actual = re.search(pattern, linux, re.MULTILINE)
            self.assertIsNotNone(expected, key)
            self.assertIsNotNone(actual, key)
            self.assertEqual(actual[1], expected[1], key)

    def test_per_user_install_and_uninstall(self):
        self.fixture()
        home = self.root / str(Path.home()).lstrip('/')
        home.mkdir(parents=True)
        os.chown(home, 1000, 1000)
        self.run_script('install.sh', user=True)
        dst = home / '.config/obs-studio/plugins/obs-vdoninja'
        self.assertTrue((dst / 'bin/64bit/obs-vdoninja.so').exists())
        self.run_script('uninstall.sh', '--remove-data', user=True)
        self.assertFalse((dst / 'bin/64bit/obs-vdoninja.so').exists())
        self.assertFalse((dst / 'bin/64bit/obs-vdoninja').exists())
        self.assertFalse((dst / 'data').exists())

    def test_per_user_xdg_config_round_trip(self):
        self.fixture()
        config = self.root / 'custom config'
        config.mkdir()
        os.chown(config, 1000, 1000)
        env = {'XDG_CONFIG_HOME': '/custom config'}
        self.run_script('install.sh', user=True, extra_env=env)
        dst = config / 'obs-studio/plugins/obs-vdoninja'
        self.assertTrue((dst / 'bin/64bit/obs-vdoninja.so').exists())
        self.run_script('uninstall.sh', user=True, extra_env=env)
        self.assertFalse((dst / 'bin/64bit/obs-vdoninja.so').exists())
        self.assertFalse((dst / 'bin/64bit/obs-vdoninja').exists())
        self.assertTrue((dst / 'data/sentinel.txt').exists())
        self.run_script('uninstall.sh', '--remove-data', user=True, extra_env=env)
        self.assertFalse((dst / 'data').exists())

    def test_missing_dependency_fails_before_copying(self):
        self.fixture()
        shutil.rmtree(self.plugin_dir / 'obs-vdoninja')
        self.assertIn('not found', self.run_script('install.sh', success=False))
        self.assertFalse((self.root / 'usr/share/obs').exists())

    def test_incompatible_obs_fails_before_copying(self):
        self.fixture()
        for version in ('OBS Studio - 30.0.2', 'OBS Studio - 32.1.0', 'OBS Studio - 33.0.0', 'unknown'):
            self.obs_version(version)
            self.assertIn('requires OBS Studio 32.2.x', self.run_script('install.sh', success=False))
        self.assertFalse((self.root / 'usr/share/obs').exists())

    def test_snap_and_missing_obs_rejected(self):
        self.fixture()
        obs = self.root / 'usr/bin/obs'
        obs.unlink()
        self.assertIn('Native OBS', self.run_script('install.sh', success=False))
        snap = self.root / 'usr/bin/snap'
        snap.touch()
        snap.chmod(0o755)
        obs.symlink_to('/usr/bin/snap')
        self.assertIn('not Snap', self.run_script('install.sh', success=False))

    def test_loader_gate_accepts_private_runtime_and_rejects_missing_one(self):
        self.fixture()
        script = REPO / 'scripts/validate-linux-package.py'
        subprocess.run(['python3', str(script), str(self.plugin)], check=True)
        shutil.rmtree(self.plugin_dir / 'obs-vdoninja')
        result = subprocess.run(['python3', str(script), str(self.plugin)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)

    def test_loader_gate_rejects_wrong_obs_api(self):
        self.fixture()
        result = subprocess.run(['python3', str(REPO / 'scripts/validate-linux-package.py'),
                                 str(self.plugin), '--obs-version', '30.0.2'], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'OBS API version mismatch', result.stderr)

    def test_loader_gate_rejects_unresolved_symbols(self):
        self.fixture()
        subprocess.run(['cc', '-shared', '-fPIC', '-x', 'c', '-', '-o', str(self.plugin),
                        '-L' + str(self.plugin_dir / 'obs-vdoninja'), '-l:libdatachannel.so.0.20',
                        '-Wl,-rpath,$ORIGIN/obs-vdoninja'],
                       input='extern int dependency(void), missing_symbol(void);\n'
                             'int use(void) { return dependency() + missing_symbol(); }\n'
                             'unsigned obs_module_ver(void) { return 0x20020000; }',
                       text=True, check=True)
        result = subprocess.run(['python3', str(REPO / 'scripts/validate-linux-package.py'),
                                 str(self.plugin)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'undefined symbol: missing_symbol', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
