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
import report_utils

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
            kpi = Path(tmp) / 'bootloader-kpi.tsv'
            kpi.write_text(
                'bl_start\t32768\n'
                'bl_load_kernel\t65536\n'
                'bl_end\t98304\n'
                'mpm_freq\t32768\n',
                encoding='utf-8',
            )
            qcom = analyze_probe.read_qcom_kpi(kpi)
        self.assertEqual(qcom['QCOM_BL_TOTAL_MS'], 2000)
        self.assertEqual(qcom['QCOM_BL_PRE_MS'], 1000)
        self.assertEqual(qcom['QCOM_BL_KL_MS'], 1000)
        self.assertEqual(qcom['QCOM_MPM_FREQ_HZ'], 32768)

    def test_qcom_kpi_delta_uefilog_and_mtk_bootprof(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            kpi = tmp_path / 'bootloader-kpi.tsv'
            kpi.write_text(
                'bl_start\t98943\n'
                'bl_end\t154935\n'
                'bl_load_kernel\t1796\n'
                'kernel_mpm\t233685\n'
                'mpm_freq\t32768\n',
                encoding='utf-8',
            )
            qcom = analyze_probe.read_qcom_kpi(kpi)
            self.assertEqual(qcom['QCOM_BL_KL_MS'], 55)
            self.assertEqual(qcom['QCOM_KERNEL_HANDOFF_MS'], 2403)
            self.assertEqual(qcom['QCOM_PRE_BL_MS'], 3020)

            uefi = tmp_path / 'bootloader-uefi.txt'
            uefi.write_text(
                'APBL Exit: 140186\n'
                'B -    150030 - SBL1, Start\n'
                'B -   1476749 - SBL1, End\n'
                'D -   1329464 - SBL1, Delta\n'
                'UEFI Start     [ 1744]\n'
                'UEFI Total : 1264 ms\n'
                'Load Image boot_a total time: 55 ms\n'
                'Apply Overlay total time: 300 ms\n'
                'Update Device Tree total time: 46 ms\n'
                'Shutting Down UEFI Boot Services: 4412 ms\n',
                encoding='utf-8',
            )
            uefi_bl = analyze_probe.read_uefilog_bl(uefi)
            self.assertEqual(uefi_bl['UEFI_PBL_MS'], 140)
            self.assertEqual(uefi_bl['UEFI_SBL1_MS'], 1329)
            self.assertEqual(uefi_bl['UEFI_DXE_MS'], 1264)
            self.assertEqual(uefi_bl['UEFI_ABL_MS'], 1404)
            self.assertEqual(uefi_bl['UEFI_KL_BOOT_MS'], 55)
            self.assertEqual(uefi_bl['UEFI_OVL_MS'], 300)
            self.assertEqual(uefi_bl['UEFI_DT_MS'], 46)
            self.assertEqual(uefi_bl['UEFI_TOTAL_MS'], 4412)

            bp = tmp_path / 'bootloader-bootprof.txt'
            bp.write_text(
                'PL Boot Time : 412\n'
                'LK Boot Time : 980\n',
                encoding='utf-8',
            )
            mtk_bl = analyze_probe.read_mtk_bootprof(bp)
            self.assertEqual(mtk_bl['MTK_PL_MS'], 412)
            self.assertEqual(mtk_bl['MTK_LK_MS'], 980)
            self.assertEqual(mtk_bl['MTK_BL_TOTAL_MS'], 1392)

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

    def test_read_proc_samples_and_hotspots(self):
        proc_text = '\n'.join([
            '# sample=0 uptime=12.00 tot=10000 idle=8000 iow=200 mem_avail_mb=5000 mem_tot_mb=11468',
            '100\t1\tsystem_server\tS\t400',
            '200\t1\tsurfaceflinger\tS\t200',
            '# sample=1 uptime=13.00 tot=11000 idle=8400 iow=250 mem_avail_mb=4700 mem_tot_mb=11468',
            '100\t1\tsystem_server\tR\t650',
            '200\t1\tsurfaceflinger\tS\t250',
            '350\t100\tdex2oat64\tD\t150',
        ])
        with tempfile.TemporaryDirectory() as tmp:
            tsv = Path(tmp) / 'proc.tsv'
            tsv.write_text(proc_text, encoding='utf-8')
            samples = analyze_probe.read_proc_samples(tsv)
        self.assertEqual(len(samples), 2)
        intervals, top_overall, d_events = analyze_probe.compute_proc_hotspots(samples)
        self.assertEqual(intervals[0]['mode'], 'baseline')
        self.assertEqual(intervals[1]['mode'], 'delta')
        self.assertAlmostEqual(intervals[1]['cpu_busy_pct'], 60.0)
        self.assertAlmostEqual(intervals[1]['iowait_pct'], 5.0)
        self.assertEqual(intervals[1]['d_procs'], ['dex2oat64'])
        self.assertEqual(top_overall[0]['comm'], 'system_server')
        self.assertEqual(top_overall[0]['total_ticks'], 250)
        self.assertEqual(top_overall[1]['comm'], 'dex2oat64')
        self.assertEqual(top_overall[1]['total_ticks'], 150)
        self.assertEqual(d_events, [(13.0, 'dex2oat64')])


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

    def test_ab_compare_and_html_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            for label, boost in (('runA', 0), ('runB', 300)):
                d = tmp_path / label
                d.mkdir()
                (d / 'metadata.txt').write_text(
                    f'boot_id={label}\nandroid_version=17\nsdk=37\n',
                    encoding='utf-8',
                )
                (d / 'samples.txt').write_text(
                    '--- sample=0 uptime=12.00 ---\n'
                    '[ro.boot.boottime]: [2BLE:600,AVB:120]\n'
                    '[init.svc.bootanim]: [running]\n'
                    '[ro.boottime.bootanim]: [2200000000]\n'
                    '[sys.boot_completed]: [1]\n',
                    encoding='utf-8',
                )
                (d / 'boot-events.tsv').write_text(
                    f'boot_progress_start\t{10900 + boost}\n'
                    f'boot_progress_enable_screen\t{20000 + boost}\n',
                    encoding='utf-8',
                )
            run_a = report_utils.load_boot_run(tmp_path / 'runA' / 'samples.txt')
            run_b = report_utils.load_boot_run(tmp_path / 'runB' / 'samples.txt')
            comp = report_utils.compare_runs(run_a, run_b)
            enable_row = next(r for r in comp['milestones'] if r['name'] == 'events:boot_progress_enable_screen')
            self.assertEqual(enable_row['delta_ms'], 300.0)
            text = report_utils.format_comparison_text(run_a, run_b)
            self.assertIn('+300.0 ms', text)
            html_out = report_utils.render_html_report(run_a, run_b)
            self.assertIn('<!DOCTYPE html>', html_out)
            self.assertIn('boot_progress_enable_screen', html_out)
            self.assertIn('A / B 启动对比', html_out)


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

    def test_bootfont_glyph_table_consistency(self):
        src = (ROOT / 'core' / 'canvas.cpp').read_text(encoding='utf-8')
        chars_match = re.search(r'kFontChars\s*=\s*"([^"]+)"', src)
        self.assertIsNotNone(chars_match)
        characters = chars_match.group(1)
        glyphs_block = re.search(r'kFontGlyphs\[\]\s*=\s*\{(.*?)\};', src, re.DOTALL)
        self.assertIsNotNone(glyphs_block)
        glyphs = re.findall(r'"([0-9A-F]{14})"', glyphs_block.group(1))
        self.assertEqual(len(characters), len(glyphs))
        self.assertIn('?', characters)

    def test_native_core_and_webui_structure(self):
        core_files = sorted((ROOT / 'core').glob('*.h')) + sorted((ROOT / 'core').glob('*.cpp'))
        core_cpp = '\n'.join(p.read_text(encoding='utf-8') for p in core_files)
        rules_conf = (ROOT / 'module' / 'rules.conf').read_text(encoding='utf-8')
        for marker in (
            'CRITICAL PATH', 'EVENTS', 'M=MOD', '*:E', 'android_id', 'bootscope_rules.conf',
            '1280', '680', '760', '920', 'read_disk_io', 'read_top_cpu', 'read_mem_zram',
            'read_cpu_freq_line', 'RKNS_3gui13LayerMetadataEPj', 'ENS_3gui13LayerMetadataEPj',
            'getPhysicalDisplayTokenENS_17PhysicalDisplayIdE', 'getPhysicalDisplayTokenEm',
            'Bootloader start count', 'parse_qcom_uefilog_file', 'parse_boottime_props',
            'parse_mtk_bootprof_file', '__system_property_foreach', 'cmd_list_json',
            'cmd_inspect_json', 'cmd_compare_json', 'cmd_rules_set_json',
            'BOOTSCOPE / BOOTLOADER', 'ro.surface_flinger.primary_display_orientation', 'rot_quad',
            'sanitize_trace_text', 'errors_total', 'getrusage', 'self_cpu_ms', 'self_rss_kb',
            'bootscope_early', 'cmd_logs_json', 'logs-json', 'log_counts', '--order', '"critical"',
            'redact_sensitive_text', 'is_caused_by_line', 'trace_text',
        ):
            self.assertIn(marker, core_cpp)
        self.assertFalse((ROOT / 'display').exists())
        self.assertIn('ignore_cmd=', rules_conf)
        self.assertIn('module_key=bootscope', rules_conf)
        self.assertIn('redact_key=android_id', rules_conf)

        web_pkg = (ROOT / 'webui' / 'package.json').read_text(encoding='utf-8')
        web_bridge = (ROOT / 'webui' / 'src' / 'bridge.ts').read_text(encoding='utf-8')
        web_format = (ROOT / 'webui' / 'src' / 'format.ts').read_text(encoding='utf-8')
        web_css = (ROOT / 'webui' / 'src' / 'styles.css').read_text(encoding='utf-8')
        web_app = (ROOT / 'webui' / 'src' / 'App.tsx').read_text(encoding='utf-8')
        self.assertIn('"preact"', web_pkg)
        self.assertIn('"vite"', web_pkg)
        self.assertIn('window.ksu', web_bridge)
        self.assertIn('bridge.exec(', web_bridge)
        self.assertNotIn('const execFn', web_bridge)
        self.assertIn('list-json', web_bridge)
        self.assertIn('inspect-json', web_bridge)
        self.assertIn('compare-json', web_bridge)
        self.assertIn('logs-json', web_bridge)
        self.assertIn('shQuote', web_bridge)
        self.assertIn('grid-template-columns: minmax(0, 1fr)', web_css)
        self.assertNotIn('grid-template-columns: 1fr;', web_css)
        for css_tok in (
            'overflow-x: clip', '-webkit-text-size-adjust: 100%', '.log-scroll',
            'width: max-content', 'overscroll-behavior-x: contain',
        ):
            self.assertIn(css_tok, web_css)
        for app_tok in ('log-scroll', 'kv-scroll', 'is-hidden'):
            self.assertIn(app_tok, web_app)
        self.assertNotIn('scroll-hint', web_app)
        self.assertNotIn('左右滑动', web_app)
        web_run_select = (ROOT / 'webui' / 'src' / 'components' / 'RunSelect.tsx').read_text(encoding='utf-8')
        web_replay_bar = (ROOT / 'webui' / 'src' / 'components' / 'ReplayBar.tsx').read_text(encoding='utf-8')
        web_compare = (ROOT / 'webui' / 'src' / 'components' / 'CompareView.tsx').read_text(encoding='utf-8')
        web_logs = (ROOT / 'webui' / 'src' / 'components' / 'LogsView.tsx').read_text(encoding='utf-8')
        self.assertIn('role="combobox"', web_run_select)
        self.assertIn('role="listbox"', web_run_select)
        self.assertNotIn('<select', web_replay_bar)
        self.assertNotIn('<select', web_compare)
        self.assertIn('log-group-head', web_logs)
        self.assertIn('log-scroll', web_logs)
        self.assertNotIn('左右滑动', web_logs)
        self.assertIn('copyTextToClipboard', web_format)
        self.assertIn('is-copied', web_logs)
        self.assertIn('is-copied', web_css)
        self.assertIn('timeline-stage-divider', web_logs)
        self.assertIn('时间线', web_logs)
        self.assertTrue((ROOT / 'webui' / 'src' / 'components' / 'PsiCpuCard.tsx').is_file())
        self.assertTrue((ROOT / 'webui' / 'src' / 'components' / 'BootTrendCard.tsx').is_file())
        web_psi = (ROOT / 'webui' / 'src' / 'components' / 'PsiCpuCard.tsx').read_text(encoding='utf-8')
        json_api_cpp = (ROOT / 'core' / 'json_api.cpp').read_text(encoding='utf-8')
        self.assertIn('PsiCpuCard', web_app)
        self.assertIn('BootTrendCard', web_app)
        self.assertIn('bl_deltas', web_compare)
        self.assertIn('nearestChartIndex', web_format)
        self.assertIn('buildMonotoneCubic', web_format)
        self.assertIn('svgXFromPointer', web_format)
        self.assertIn('buildMonotoneCubic', web_psi)
        self.assertIn('psi-cpu-fill', web_psi)
        self.assertNotIn('<polyline', web_psi)
        self.assertIn('trace_metrics', json_api_cpp)
        self.assertIn('chart-ruler', web_css)
        self.assertIn('chart-samples', web_css)

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
        self.assertIn('/metadata/vold/ksu', service_sh)
        self.assertIn('bootscope_rules.conf', service_sh)
        self.assertNotIn('app_process', service_sh)

        customize_sh = (ROOT / 'module' / 'customize.sh').read_text(encoding='utf-8')
        self.assertIn('bootscope', customize_sh)
        self.assertIn('bootscope_rules.conf', customize_sh)
        self.assertIn('KSU', customize_sh)
        self.assertIn('arm64-v8a', customize_sh)
        self.assertIn('KSU_HAS_HOT_INSTALL', customize_sh)
        self.assertIn('MODULE_HOT_INSTALL_REQUEST=true', customize_sh)
        self.assertIn('/metadata/vold/ksu/bootscope early', customize_sh)
        self.assertNotRegex(customize_sh, r'(?m)^\s*exit\b')

        uninstall_sh = (ROOT / 'module' / 'uninstall.sh').read_text(encoding='utf-8')
        self.assertIn('/metadata/vold/ksu/bootscope', uninstall_sh)
        self.assertIn('bootscope_rules.conf', uninstall_sh)

        rc = (ROOT / 'module' / 'initrc' / 'bootscope.rc').read_text(encoding='utf-8')
        self.assertIn('service bootscope_early /metadata/vold/ksu/bootscope early', rc)
        self.assertIn('on late-fs', rc)

        probe_sh = (ROOT / 'module' / 'probe.sh').read_text(encoding='utf-8')
        display_sh = (ROOT / 'module' / 'display.sh').read_text(encoding='utf-8')
        action_sh = (ROOT / 'module' / 'action.sh').read_text(encoding='utf-8')
        self.assertIn('bootscope" collect', probe_sh)
        self.assertIn('.display.lock', display_sh)
        self.assertIn('early_native', display_sh)
        self.assertIn('late_native', display_sh)
        self.assertIn('replay latest --bl --pre-sf', action_sh)
        self.assertIn('summary latest', action_sh)

    def test_build_module_zip_layout(self):
        cmd = [sys.executable, str(ROOT / 'build_module.py')]
        if (ROOT / 'dist' / 'webroot' / 'index.html').is_file():
            cmd.append('--skip-webui')
        subprocess.run(cmd, check=True)
        version = package_utils.read_module_version(ROOT / 'module' / 'module.prop')
        archive_path = ROOT / 'dist' / f'bootscope-{version}.zip'
        self.assertTrue(archive_path.is_file())
        with ZipFile(archive_path, 'r') as archive:
            names = set(archive.namelist())
            for req in (
                'module.prop', 'action.sh', 'service.sh', 'probe.sh', 'display.sh',
                'customize.sh', 'uninstall.sh', 'rules.conf', 'initrc/bootscope.rc',
                'bootscope', 'webroot/index.html',
            ):
                self.assertIn(req, names)
            self.assertNotIn('classes.dex', names)
            self.assertEqual(archive.getinfo('bootscope').external_attr >> 16, 0o100755)
            for sh in ('action.sh', 'service.sh', 'probe.sh', 'display.sh'):
                self.assertEqual(archive.getinfo(sh).external_attr >> 16, 0o100755)
                self.assertNotIn(b'\r\n', archive.read(sh))


if __name__ == '__main__':
    unittest.main()
