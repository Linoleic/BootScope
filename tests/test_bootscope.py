"""Unit and packaging regression tests for BootScope."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from zipfile import ZipFile

import analyze_probe
import bootscope
import build_display
import package_utils
import replay

ROOT = Path(__file__).resolve().parent.parent


class BootScopeParsingTests(unittest.TestCase):
    def test_parse_properties_and_build_snapshot(self):
        raw = '\n'.join([
            '[init.svc.surfaceflinger]: [running]',
            '[init.svc_debug_pid.surfaceflinger]: [512]',
            '[ro.boottime.surfaceflinger]: [2259000000]',
            '[init.svc.custom]: [weird_state]',
            '[ro.boottime.custom]: [not_a_number]',
            '[sys.boot_completed]: [1]',
            'malformed line ignored',
        ])
        props = bootscope.parse_properties(raw)
        self.assertEqual(props['init.svc.surfaceflinger'], 'running')
        snapshot = bootscope.build_snapshot(props)
        self.assertTrue(snapshot['boot_completed'])
        self.assertEqual(snapshot['boot_completed_raw'], '1')
        self.assertNotIn('surfaceflinger_debug_pid', snapshot['services'])
        self.assertEqual(
            snapshot['services']['surfaceflinger'],
            {'state': 'running', 'first_start_boottime_ns': 2259000000},
        )
        self.assertEqual(
            snapshot['services']['custom'],
            {'state': 'unknown', 'first_start_boottime_ns': None},
        )

    def test_read_samples_and_transitions(self):
        sample_text = '\n'.join([
            '--- sample=0 uptime=11.71 ---',
            '[init.svc.bootanim]: [running]',
            '[ro.boottime.bootanim]: [2256000000]',
            '[service.bootanim.exit]: [0]',
            '--- sample=1 uptime=21.37 ---',
            '[init.svc.bootanim]: [stopped]',
            '[ro.boottime.bootanim]: [2256000000]',
            '[init.svc.artd]: [running]',
            '[ro.boottime.artd]: [11977000000]',
            '[service.bootanim.exit]: [1]',
            '[sys.boot_completed]: [1]',
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'samples.txt'
            path.write_text(sample_text, encoding='utf-8')
            samples = analyze_probe.read_samples(path)

        self.assertEqual(len(samples), 2)
        self.assertEqual(samples[0][0], (0, 11.71))
        self.assertEqual(samples[0][1]['bootanim_exit_raw'], '0')
        self.assertIsNone(samples[0][1]['boot_completed_raw'])
        self.assertEqual(samples[1][1]['boot_completed_raw'], '1')

        transitions = analyze_probe.collect_transitions(samples)
        self.assertEqual(
            transitions,
            [
                (21.37, 'artd', 'absent', 'running', 11977000000),
                (21.37, 'bootanim', 'running', 'stopped', 2256000000),
            ],
        )

    def test_read_boot_events_filters_non_ascii_and_invalid(self):
        with tempfile.TemporaryDirectory() as tmp:
            tsv = Path(tmp) / 'boot-events.tsv'
            tsv.write_text(
                'boot_progress_start\t10940\n'
                'bad_line_no_tab\n'
                'boot_progress_bad\t12a\n'
                'boot_progress_unicode\t١٢٣\n'
                'wm_boot_animation_done\t21139\n',
                encoding='utf-8',
            )
            events = analyze_probe.read_boot_events(tsv)
        self.assertEqual(
            events,
            [('boot_progress_start', 10940), ('wm_boot_animation_done', 21139)],
        )


class ReplayTests(unittest.TestCase):
    def test_replay_tracks_non_core_service_changes(self):
        sample_text = '\n'.join([
            '--- sample=0 uptime=11.71 ---',
            '[init.svc.surfaceflinger]: [running]',
            '[init.svc.bootanim]: [running]',
            '[service.bootanim.exit]: [0]',
            '--- sample=1 uptime=12.85 ---',
            '[init.svc.surfaceflinger]: [running]',
            '[init.svc.bootanim]: [running]',
            '[init.svc.artd]: [running]',
            '[ro.boottime.artd]: [11977000000]',
            '[service.bootanim.exit]: [0]',
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'samples.txt'
            path.write_text(sample_text, encoding='utf-8')
            samples = analyze_probe.read_samples(path)
        playback = list(replay.frames(samples))
        self.assertEqual(len(playback), 2)
        self.assertIn('Waiting for a second sample', playback[0][1])
        self.assertIn('Changed services (other)', playback[1][1])
        self.assertIn('artd: absent → running', playback[1][1])


class PackagingAndModuleTests(unittest.TestCase):
    def test_package_utils_writes_lf_and_unix_modes(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            src_sh = tmp_path / 'service.sh'
            src_sh.write_bytes(b'#!/system/bin/sh\r\necho ok\r\n')
            zip_path = tmp_path / 'out.zip'
            with ZipFile(zip_path, 'w') as archive:
                package_utils.write_text_file(archive, 'service.sh', src_sh, 0o100755)
            with ZipFile(zip_path, 'r') as archive:
                info = archive.getinfo('service.sh')
                self.assertEqual(info.create_system, 3)
                self.assertEqual(info.external_attr >> 16, 0o100755)
                self.assertEqual(archive.read('service.sh'), b'#!/system/bin/sh\necho ok\n')

    def test_build_display_sdk_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp)
            for api in ('android-30', 'android-35', 'android-37.0'):
                jar = sdk / 'platforms' / api / 'android.jar'
                jar.parent.mkdir(parents=True)
                jar.write_bytes(b'')
            for ver in ('34.0.0', '36.1.0'):
                exe_name = 'd8.bat' if build_display.os.name == 'nt' else 'd8'
                d8 = sdk / 'build-tools' / ver / exe_name
                d8.parent.mkdir(parents=True)
                d8.write_bytes(b'')

            self.assertEqual(
                build_display.resolve_android_jar(sdk, None).parent.name,
                'android-37.0',
            )
            self.assertEqual(
                build_display.resolve_d8(sdk, None).parent.name,
                '36.1.0',
            )

    def test_bootfont_glyph_table_consistency(self):
        src = (ROOT / 'display' / 'BootFont.java').read_text(encoding='utf-8')
        chars_match = re.search(r'CHARACTERS\s*=\s*"([^"]+)"', src)
        self.assertIsNotNone(chars_match)
        characters = chars_match.group(1)
        glyphs_block = re.search(r'GLYPHS\s*=\s*\{(.*?)\};', src, re.DOTALL)
        self.assertIsNotNone(glyphs_block)
        glyphs = re.findall(r'"([0-9A-F]{14})"', glyphs_block.group(1))
        self.assertEqual(len(characters), len(glyphs))
        self.assertIn('?', characters)

    def test_display_service_sh_is_non_blocking(self):
        service_sh = (ROOT / 'display' / 'service.sh').read_text(encoding='utf-8')
        self.assertIn('run.sh', service_sh)
        self.assertIn('&', service_sh)
        self.assertNotIn('app_process', service_sh)

    def test_build_module_produces_valid_zip(self):
        subprocess.run(['python', str(ROOT / 'build_module.py')], check=True)
        version = package_utils.read_module_version(ROOT / 'module' / 'module.prop')
        archive_path = ROOT / 'dist' / f'bootscope-probe-{version}.zip'
        self.assertTrue(archive_path.is_file())
        with ZipFile(archive_path, 'r') as archive:
            self.assertEqual(sorted(archive.namelist()), ['module.prop', 'probe.sh', 'service.sh'])
            for name in ('probe.sh', 'service.sh'):
                self.assertEqual(archive.getinfo(name).external_attr >> 16, 0o100755)
                self.assertNotIn(b'\r\n', archive.read(name))


if __name__ == '__main__':
    unittest.main()
