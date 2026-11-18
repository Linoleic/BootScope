"""Unit and packaging regression tests for BootScope."""
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from zipfile import ZipFile

import analyze_probe
import bootscope
import build_module
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

    def test_extract_init_boottime_and_read_init_events(self):
        props = {
            'ro.boottime.init': '570269002',
            'ro.boottime.init.first_stage': '622935058',
            'ro.boottime.init.selinux': '116764364',
            'ro.boottime.init.modules': '407',
            'ro.boottime.init.mount_all.late': '417',
            'ro.boottime.init.fsck.data': '154',
            'ro.boottime.init.shared_modem_platform': '10809729659',
            'ro.boottime.init_display': '1978278971',
        }
        stages = analyze_probe.extract_init_boottime(props)
        names = [name for name, _, _, _ in stages]
        self.assertIn('init.first_stage', names)
        self.assertIn('init.modules', names)
        self.assertNotIn('init.shared_modem_platform', names)
        self.assertNotIn('init_display', names)

        with tempfile.TemporaryDirectory() as tmp:
            tsv = Path(tmp) / 'init-events.tsv'
            tsv.write_text(
                '0.984145\tphase\tkernel_modules\t179_modules_407ms\n'
                '1.414118\tstart\tueventd\t\n'
                '1.618918\texit\tapexd-bootstrap\tstatus=0;took=0.192000s\n'
                'bad\tphase\tx\n'
                '-1.0\tstart\ty\n'
                '2.0\tunknown\tz\n',
                encoding='utf-8',
            )
            events = analyze_probe.read_init_events(tsv)
        self.assertEqual(len(events), 3)
        self.assertEqual(analyze_probe.parse_exit_detail(events[2][3]), ('0', 0.192))

    def test_parse_bootloader_boottime_and_psi_intervals(self):
        props = {
            'ro.boot.boottime': '0BLE:45,2BLE:673,AVB:125,KL:13',
            'ro.boot.boottime_extra': 'DISP:322,DRAM_TRAIN:133',
        }
        bl = bootscope.parse_bootloader_boottime(props)
        self.assertEqual(bl['2BLE'], 673)
        self.assertEqual(bl['AVB'], 125)
        self.assertEqual(bl['DISP'], 322)
        self.assertEqual(bl['DRAM_TRAIN'], 133)

        with tempfile.TemporaryDirectory() as tmp:
            tsv = Path(tmp) / 'psi.tsv'
            tsv.write_text(
                '0\t12.00\t1000000\t0\t12.50\t200000\t50000\t3.00\t10000\t0\t0.00\n'
                '1\t13.00\t1250000\t0\t14.00\t700000\t150000\t5.00\t10000\t0\t0.00\n'
                'bad\tline\n',
                encoding='utf-8',
            )
            rows = analyze_probe.read_psi_samples(tsv)
        self.assertEqual(len(rows), 2)
        intervals = analyze_probe.compute_psi_intervals(rows)
        self.assertEqual(intervals[0]['mode'], 'avg10')
        self.assertAlmostEqual(intervals[0]['cpu_some_pct'], 12.5)
        self.assertEqual(intervals[1]['mode'], 'delta')
        self.assertAlmostEqual(intervals[1]['cpu_some_pct'], 25.0)
        self.assertAlmostEqual(intervals[1]['io_some_pct'], 50.0)
        self.assertAlmostEqual(intervals[1]['io_full_pct'], 10.0)


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

    def test_replay_prepends_dmesg_init_frames(self):
        sample_text = '\n'.join([
            '--- sample=0 uptime=11.71 ---',
            '[init.svc.ueventd]: [running]',
            '[init.svc.surfaceflinger]: [running]',
            '[init.svc.bootanim]: [running]',
            '[sys.boot_completed]: [1]',
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'samples.txt'
            path.write_text(sample_text, encoding='utf-8')
            samples = analyze_probe.read_samples(path)
        init_events = [
            (0.984, 'phase', 'kernel_modules', '179_modules_407ms'),
            (1.414, 'start', 'ueventd', ''),
            (2.116, 'start', 'surfaceflinger', ''),
        ]
        playback = list(replay.frames(samples, init_events=init_events))
        self.assertGreater(len(playback), 1)
        self.assertLess(playback[0][0], 11.71)
        self.assertIn('dmesg init 1', playback[0][1])
        self.assertIn('Sample 1/1', playback[-1][1])

    def test_replay_renders_bootloader_and_psi(self):
        sample_text = '\n'.join([
            '--- sample=0 uptime=11.71 ---',
            '[ro.boot.boottime]: [2BLE:673,AVB:125,KL:13]',
            '[init.svc.bootanim]: [running]',
            '[sys.boot_completed]: [0]',
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'samples.txt'
            path.write_text(sample_text, encoding='utf-8')
            samples = analyze_probe.read_samples(path)
        psi_intervals = [{
            'sample': 0,
            'uptime': 11.71,
            'cpu_some_pct': 18.5,
            'io_some_pct': 42.0,
            'io_full_pct': 5.0,
            'mem_some_pct': 0.0,
            'mem_full_pct': 0.0,
            'mode': 'delta',
        }]
        playback = list(replay.frames(samples, psi_intervals=psi_intervals))
        self.assertIn('Bootloader (raw)', playback[0][1])
        self.assertIn('2BLE=673', playback[0][1])
        self.assertIn('PSI (delta)  cpu=18.5%  io=42.0%  mem=0.0%', playback[0][1])


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

    def test_build_module_sdk_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp)
            for api in ('android-30', 'android-35', 'android-37.0'):
                jar = sdk / 'platforms' / api / 'android.jar'
                jar.parent.mkdir(parents=True)
                jar.write_bytes(b'')
            for ver in ('34.0.0', '36.1.0'):
                exe_name = 'd8.bat' if build_module.os.name == 'nt' else 'd8'
                d8 = sdk / 'build-tools' / ver / exe_name
                d8.parent.mkdir(parents=True)
                d8.write_bytes(b'')

            self.assertEqual(
                build_module.resolve_android_jar(sdk, None).parent.name,
                'android-37.0',
            )
            self.assertEqual(
                build_module.resolve_d8(sdk, None).parent.name,
                '36.1.0',
            )

    def test_bootfont_glyph_table_consistency(self):
        for rel in ('display/BootFont.java', 'display/bootscope_early.cpp'):
            src = (ROOT / rel).read_text(encoding='utf-8')
            chars_match = re.search(r'(?:CHARACTERS|kFontChars)\s*=\s*"([^"]+)"', src)
            self.assertIsNotNone(chars_match, rel)
            characters = chars_match.group(1)
            glyphs_block = re.search(r'(?:GLYPHS|kFontGlyphs\[\])\s*=\s*\{(.*?)\};', src, re.DOTALL)
            self.assertIsNotNone(glyphs_block, rel)
            glyphs = re.findall(r'"([0-9A-F]{14})"', glyphs_block.group(1))
            self.assertEqual(len(characters), len(glyphs), rel)
            self.assertIn('?', characters, rel)

    def test_build_module_ndk_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp)
            clang_name = 'aarch64-linux-android33-clang++' + ('.cmd' if build_module.os.name == 'nt' else '')
            for ver in ('25.2.9519653', '26.3.11579264'):
                clang = sdk / 'ndk' / ver / 'toolchains' / 'llvm' / 'prebuilt' / 'host-x86_64' / 'bin' / clang_name
                clang.parent.mkdir(parents=True)
                clang.write_bytes(b'')
            resolved = build_module.resolve_ndk(sdk, None)
            self.assertEqual(resolved.name, '26.3.11579264')
            clang_path, _ = build_module.resolve_ndk_clang(resolved)
            self.assertTrue(clang_path.is_file())

    def test_unified_module_scripts_structure(self):
        service_sh = (ROOT / 'module' / 'service.sh').read_text(encoding='utf-8')
        self.assertIn('probe.sh', service_sh)
        self.assertIn('display.sh', service_sh)
        self.assertIn('disable_display', service_sh)
        self.assertIn('&', service_sh)
        self.assertNotIn('app_process', service_sh)

        customize_sh = (ROOT / 'module' / 'customize.sh').read_text(encoding='utf-8')
        self.assertIn('bootscope_early', customize_sh)
        self.assertIn('KSU', customize_sh)
        self.assertIn('arm64-v8a', customize_sh)
        self.assertNotRegex(customize_sh, r'(?m)^\s*exit\b')

        rc = (ROOT / 'module' / 'initrc' / 'bootscope.rc').read_text(encoding='utf-8')
        self.assertIn('service bootscope_early /metadata/vold/ksu/bootscope_early', rc)
        self.assertIn('on late-fs', rc)

        probe_sh = (ROOT / 'module' / 'probe.sh').read_text(encoding='utf-8')
        display_sh = (ROOT / 'module' / 'display.sh').read_text(encoding='utf-8')
        self.assertIn('BASE=/data/adb/bootscope', probe_sh)
        self.assertIn('BASE=/data/adb/bootscope', display_sh)
        self.assertIn('.probe.lock', probe_sh)
        self.assertIn('init-events.tsv', probe_sh)
        self.assertIn('psi.tsv', probe_sh)
        self.assertIn('ro\\.boot\\.boottime', probe_sh)
        self.assertIn('dmesg', probe_sh)
        self.assertIn('.display.lock', display_sh)
        self.assertIn('early_native', display_sh)
        self.assertIn('first_frame_submitted_ms=', display_sh)
        self.assertIn('unsupported_sdk', display_sh)

    def test_build_module_headless_zip(self):
        subprocess.run([sys.executable, str(ROOT / 'build_module.py'), '--no-display'], check=True)
        version = package_utils.read_module_version(ROOT / 'module' / 'module.prop')
        archive_path = ROOT / 'dist' / f'bootscope-{version}-headless.zip'
        self.assertTrue(archive_path.is_file())
        with ZipFile(archive_path, 'r') as archive:
            self.assertEqual(sorted(archive.namelist()), ['module.prop', 'probe.sh', 'service.sh'])
            for name in ('probe.sh', 'service.sh'):
                self.assertEqual(archive.getinfo(name).external_attr >> 16, 0o100755)
                self.assertNotIn(b'\r\n', archive.read(name))


if __name__ == '__main__':
    unittest.main()
