#include "renderer.h"

#include "boot_sources.h"
#include "bootloader.h"
#include "canvas.h"
#include "common.h"
#include "early_trace.h"
#include "replay_source.h"
#include "rules.h"
#include "sf_gui.h"
#include "sys_metrics.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

namespace bootscope {
namespace {

void write_pid_file() {
    FILE* f = fopen(kPidPath, "w");
    if (f) {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
}

bool reexec_from_tmp_if_needed(const DisplayOptions& opts, int argc, char** argv) {
    if (opts.in_tmp) return false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--in-tmp") == 0) return false;
    }
    int src = open("/proc/self/exe", O_RDONLY);
    if (src < 0) return false;
    std::string stage = std::string(kTmpExecPath) + ".tmp";
    int dst = open(stage.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dst < 0) {
        close(src);
        return false;
    }
    char buf[16384];
    ssize_t n;
    bool ok = true;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst, buf + off, (size_t)(n - off));
            if (w <= 0) {
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok) break;
    }
    if (n < 0) ok = false;
    close(src);
    if (fchmod(dst, 0755) != 0) ok = false;
    close(dst);
    if (!ok || rename(stage.c_str(), kTmpExecPath) != 0) {
        unlink(stage.c_str());
        return false;
    }

    std::vector<char*> next_argv;
    next_argv.push_back((char*)kTmpExecPath);
    next_argv.push_back((char*)"early");
    for (int i = 1; i < argc; i++) {
        if (i == 1 && strcmp(argv[i], "early") == 0) continue;
        next_argv.push_back(argv[i]);
    }
    next_argv.push_back((char*)"--in-tmp");
    next_argv.push_back(nullptr);
    execv(kTmpExecPath, next_argv.data());
    return false;
}

struct RenderContext {
    bool preview = false;
    bool replay = false;
    bool replay_bl_phase = false;
    bool replay_pre_sf_phase = false;
    int64_t bl_por_ms = 0;
    int frame_idx = 0;
    int64_t now_ms = 0;
    double uptime_s = 0.0;
    const Geometry* geom = nullptr;
    const DeviceInfo* dev_info = nullptr;
    const StageInfo* stage = nullptr;
    const PhaseNode* phases = nullptr;
    int expanded_idx = -1;
    std::string exp_line1;
    std::string exp_line2;
    uint32_t exp_col2 = kColorMuted;
    std::string max_init_svc;
    int64_t max_init_dur_ms = 0;
    std::string psi_compact;
    int cpu_pct = -1;
    int io_pct = -1;
    const SysMetrics* sys = nullptr;
    const std::string* cur_states = nullptr;
    const int64_t* svc_start_ms = nullptr;
    const char* const* short_names = nullptr;
    int svc_count = 0;
    const std::vector<std::pair<std::string, int64_t>>* init_ms = nullptr;
    std::string bootloader_line;
    std::string footer;
    std::string rules_label;
    const RuleSet* rules = nullptr;
    const std::vector<TailEntry>* tail = nullptr;
    size_t max_rows = 8;
};

void draw_resources_block(
        uint32_t* px, int stride, int rx0, int rx1, int ry, size_t rchars, const RenderContext& ctx) {
    fill_rect(px, stride, rx0, ry, rx1, ry + 2, kColorTrack);
    draw_text(px, stride, "RESOURCES", rx0, ry + 8, 2, kColorAccent);
    if (!ctx.sys->rows.empty()) {
        for (size_t i = 0; i < ctx.sys->rows.size() && i < 4; i++) {
            const MetricRow& r = ctx.sys->rows[i];
            std::string line = r.text;
            if (line.size() > rchars) line.resize(rchars);
            uint32_t col = r.alert ? kColorError : (i == 2 ? kColorMuted : kColorPrimary);
            draw_text(px, stride, line.c_str(), rx0, ry + 29 + (int)i * 21, 2, col);
        }
        return;
    }
    std::string psi_str = ctx.psi_compact.empty() ? "PSI N/A" : ctx.psi_compact;
    uint32_t psi_col = (ctx.io_pct >= 30 || ctx.cpu_pct >= 65) ? kColorError : kColorPrimary;
    std::string line_a = psi_str + "  " + ctx.sys->freq_line;
    if (line_a.size() > rchars) line_a.resize(rchars);
    draw_text(px, stride, line_a.c_str(), rx0, ry + 29, 2, psi_col);

    std::string line_b = ctx.sys->mem_line;
    if (line_b.size() > rchars) line_b.resize(rchars);
    draw_text(px, stride, line_b.c_str(), rx0, ry + 50, 2,
              ctx.sys->mem_low ? kColorError : kColorPrimary);

    std::string line_c = ctx.sys->io_line;
    if (line_c.size() > rchars) line_c.resize(rchars);
    draw_text(px, stride, line_c.c_str(), rx0, ry + 71, 2,
              ctx.sys->io_busy ? kColorError : kColorMuted);
}

void draw_tail_block(
        uint32_t* px, int stride, int tx0, int tx1, int ty, size_t max_chars, const RenderContext& ctx) {
    fill_rect(px, stride, tx0, ty, tx1, ty + 2, kColorTrack);
    draw_text(px, stride, "EVENTS", tx0, ty + 9, 2, kColorAccent);
    const char* legend = "B=BL K=INIT P=PROG S=SVC E=ERR M=MOD";
    int legend_x = tx1 - (int)strlen(legend) * 12;
    if (legend_x > tx0 + 96) {
        draw_text(px, stride, legend, legend_x, ty + 9, 2, kColorMuted);
    }
    if (ctx.tail->empty()) {
        draw_text(px, stride, "WAITING BOOT EVENTS...", tx0, ty + 31, 2, kColorMuted);
        return;
    }
    for (size_t i = 0; i < ctx.tail->size() && i < ctx.max_rows; i++) {
        const TailEntry& item = (*ctx.tail)[i];
        int row_y = ty + 31 + (int)i * 22;
        uint32_t row_color = kColorMuted;
        if (item.kind == TailKind::Stage) {
            row_color = kColorAccent;
        } else if (item.kind == TailKind::Error) {
            row_color = kColorError;
        } else if (item.kind == TailKind::ModuleError) {
            row_color = kColorAccent;
            fill_rect(px, stride, tx0 - 10, row_y, tx0 - 6, row_y + 14, kColorAccent);
        }
        std::string row_str = format_tail_row(item);
        if (row_str.size() > max_chars) row_str.resize(max_chars);
        draw_text(px, stride, row_str.c_str(), tx0, row_y, 2, row_color);
    }
}

void draw_dashboard(uint32_t* px, int stride, const RenderContext& ctx) {
    const int bw = canvas_width();
    const int bh = canvas_height();
    const bool is_land = (bw == kLandWidth);
    fill_rect(px, stride, 0, 0, bw, bh, kColorBg);
    fill_rect(px, stride, 0, 0, 8, bh, kColorAccent);

    const char* title = ctx.replay
        ? (ctx.replay_bl_phase
            ? "BOOTSCOPE / BOOTLOADER"
            : (ctx.replay_pre_sf_phase ? "BOOTSCOPE / PRE-SF" : "BOOTSCOPE / REPLAY"))
        : (ctx.preview ? "BOOTSCOPE / PREVIEW" : "BOOTSCOPE / BOOT");
    draw_text(px, stride, title, 24, 20, 3, kColorAccent);

    char mode_buf[32];
    if (ctx.geom->refresh_hz > 0) {
        snprintf(mode_buf, sizeof(mode_buf), "%dX%d@%d", ctx.geom->width, ctx.geom->height, ctx.geom->refresh_hz);
    } else {
        snprintf(mode_buf, sizeof(mode_buf), "%dX%d", ctx.geom->width, ctx.geom->height);
    }
    int mode_x = bw - 24 - (int)strlen(mode_buf) * 18;
    if (mode_x < 420) mode_x = 420;
    draw_text(px, stride, mode_buf, mode_x, 20, 3, kColorMuted);

    const int left_r = is_land ? 584 : (bw - 24);
    const size_t left_chars = is_land ? 46 : 59;
    std::string d1 = ctx.dev_info->line1;
    std::string d2 = ctx.dev_info->line2;
    if (d1.size() > left_chars) d1.resize(left_chars);
    if (d2.size() > left_chars) d2.resize(left_chars);
    draw_text(px, stride, d1.c_str(), 24, 50, 2, kColorPrimary);
    draw_text(px, stride, d2.c_str(), 24, 70, 2, kColorMuted);

    char meta_buf[48];
    if (ctx.replay_bl_phase) {
        snprintf(meta_buf, sizeof(meta_buf), "BL POR %.2fS  STG 0/%d", ctx.bl_por_ms / 1000.0, kStageTotal);
    } else {
        snprintf(meta_buf, sizeof(meta_buf), "UPTIME %.2fS  STG %d/%d", ctx.uptime_s, ctx.stage->step, kStageTotal);
    }
    draw_text(px, stride, meta_buf, 24, 92, 3, kColorPrimary);

    const int64_t phase_clock_ms = ctx.replay_bl_phase ? ctx.bl_por_ms : ctx.now_ms;
    const int bar_top = 122, bar_bottom = 136;
    const int seg_gap = 6;
    const int seg_w = ((left_r - 24) - 5 * seg_gap) / 6;
    for (int i = 0; i < 6; i++) {
        int x0 = 24 + i * (seg_w + seg_gap);
        int x1 = x0 + seg_w;
        fill_rect(px, stride, x0, bar_top, x1, bar_bottom, kColorTrack);
        if (ctx.phases[i].state == NodeState::Done) {
            fill_rect(px, stride, x0, bar_top, x1, bar_bottom, kColorAccent);
        } else if (ctx.phases[i].state == NodeState::Running) {
            int pulse_w = (seg_w / 3) + ((ctx.frame_idx % 4) * (seg_w / 6));
            if (pulse_w > seg_w) pulse_w = seg_w;
            int64_t rdur = ctx.phases[i].start_ms >= 0 ? (phase_clock_ms - ctx.phases[i].start_ms) : 0;
            uint32_t run_col = (rdur >= 6000) ? kColorError : kColorPrimary;
            fill_rect(px, stride, x0, bar_top, x0 + pulse_w, bar_bottom, run_col);
            fill_rect(px, stride, x0, bar_top, x1, bar_top + 2, kColorAccent);
            fill_rect(px, stride, x0, bar_bottom - 2, x1, bar_bottom, kColorAccent);
        }
        int lbl_len = (int)strlen(ctx.phases[i].id);
        int lbl_x = x0 + (seg_w - lbl_len * 12) / 2;
        uint32_t lbl_col = kColorMuted;
        if (ctx.phases[i].state == NodeState::Done) lbl_col = kColorAccent;
        else if (ctx.phases[i].state == NodeState::Running) lbl_col = kColorPrimary;
        draw_text(px, stride, ctx.phases[i].id, lbl_x, 142, 2, lbl_col);
    }

    fill_rect(px, stride, 24, 164, left_r, 166, kColorTrack);
    draw_text(px, stride, "CRITICAL PATH", 24, 172, 2, kColorAccent);
    std::string stage_hdr = ctx.stage->raw_ms.empty() ? ctx.stage->label : (ctx.stage->label + " " + ctx.stage->raw_ms + "MS");
    if (stage_hdr.size() > 26) stage_hdr.resize(26);
    int sh_x = left_r - (int)stage_hdr.size() * 12;
    draw_text(px, stride, stage_hdr.c_str(), sh_x, 172, 2, kColorPrimary);

    int tree_y = 194;
    for (int i = 0; i < 6; i++) {
        uint32_t row_col = kColorMuted;
        if (ctx.phases[i].state == NodeState::Done) {
            row_col = kColorAccent;
        } else if (ctx.phases[i].state == NodeState::Running) {
            int64_t rdur = ctx.phases[i].start_ms >= 0 ? (phase_clock_ms - ctx.phases[i].start_ms) : 0;
            row_col = (rdur >= 6000) ? kColorError : kColorPrimary;
        }
        std::string prow = format_phase_row(ctx.phases[i], phase_clock_ms, *ctx.rules);
        if (prow.size() > left_chars) prow.resize(left_chars);
        draw_text(px, stride, prow.c_str(), 24, tree_y, 2, row_col);
        tree_y += 20;

        if (i == ctx.expanded_idx) {
            int sub1_y = tree_y;
            int sub2_y = tree_y + 20;
            fill_rect(px, stride, 30, sub1_y - 4, 32, sub2_y + 8, kColorAccent);
            fill_rect(px, stride, 32, sub1_y + 6, 40, sub1_y + 8, kColorAccent);
            fill_rect(px, stride, 32, sub2_y + 6, 40, sub2_y + 8, kColorAccent);
            std::string el1 = ctx.exp_line1;
            std::string el2 = ctx.exp_line2;
            if (el1.size() > left_chars - 2) el1.resize(left_chars - 2);
            if (el2.size() > left_chars - 2) el2.resize(left_chars - 2);
            draw_text(px, stride, el1.c_str(), 46, sub1_y, 2, kColorPrimary);
            draw_text(px, stride, el2.c_str(), 46, sub2_y, 2, ctx.exp_col2);
            tree_y += 40;
        }
    }
    if (ctx.expanded_idx < 0) {
        std::string sum1 = "ALL PHASES COLLAPSED / COMPLETE";
        if (!ctx.max_init_svc.empty()) {
            char mbuf[48];
            snprintf(mbuf, sizeof(mbuf), "MAX-INIT:%s %.2fS", ctx.max_init_svc.c_str(), ctx.max_init_dur_ms / 1000.0);
            sum1 = sanitize_tail_text(*ctx.rules, std::string("ALL PHASES DONE  ") + mbuf, left_chars);
        }
        if (sum1.size() > left_chars) sum1.resize(left_chars);
        draw_text(px, stride, sum1.c_str(), 24, tree_y, 2, kColorAccent);
    }

    std::string bl_init_line = "INIT:";
    for (size_t i = 0; i < ctx.init_ms->size() && i < 4; i++) {
        char mb[24];
        snprintf(mb, sizeof(mb), " %s@%.1f", short_init_action((*ctx.init_ms)[i].first).c_str(),
                 (*ctx.init_ms)[i].second / 1000.0);
        bl_init_line += mb;
    }
    if (ctx.init_ms->empty()) bl_init_line = ctx.bootloader_line;
    if (bl_init_line.size() > left_chars) bl_init_line.resize(left_chars);

    std::string footer = ctx.footer;
    if (is_land) {
        draw_text(px, stride, bl_init_line.c_str(), 24, 360, 2, kColorMuted);
        draw_resources_block(px, stride, 24, left_r, 384, left_chars, ctx);

        fill_rect(px, stride, 24, 480, left_r, 482, kColorTrack);
        draw_text(px, stride, "TOP CPU", 24, 488, 2, kColorAccent);
        if (ctx.sys->top_cpu.empty()) {
            draw_text(px, stride, "SAMPLING /PROC...", 24, 510, 2, kColorMuted);
        } else {
            for (size_t i = 0; i < ctx.sys->top_cpu.size() && i < 3; i++) {
                char tb[64];
                snprintf(tb, sizeof(tb), "%zu. %-22s %5.1f%%",
                         i + 1, ctx.sys->top_cpu[i].comm.c_str(), ctx.sys->top_cpu[i].pct);
                uint32_t tc = (ctx.sys->top_cpu[i].pct >= 25.0) ? kColorError : kColorPrimary;
                draw_text(px, stride, tb, 24, 510 + (int)i * 21, 2, tc);
            }
        }

        std::string bl_sum = sanitize_tail_text(*ctx.rules, ctx.bootloader_line, left_chars);
        draw_text(px, stride, bl_sum.c_str(), 24, 584, 2, kColorMuted);
        if (footer.size() > 30) footer.resize(30);
        draw_text(px, stride, footer.c_str(), 24, 608, 2, kColorMuted);
        draw_text(px, stride, ctx.rules_label.c_str(),
                  left_r - (int)ctx.rules_label.size() * 12, 608, 2, kColorMuted);

        fill_rect(px, stride, 598, 50, 600, bh - 20, kColorTrack);

        const int rx0 = 616;
        const int rx1 = bw - 24;
        draw_text(px, stride, "SERVICES", rx0, 50, 2, kColorAccent);
        for (int i = 0; i < ctx.svc_count; i++) {
            int col = i % 2;
            int row = i / 2;
            int sx = rx0 + col * 324;
            int sy = 72 + row * 21;
            char sb[40];
            if (ctx.svc_start_ms[i] >= 0) {
                snprintf(sb, sizeof(sb), "%-4s:%-4s @%.1fS",
                         ctx.short_names[i], ctx.cur_states[i].c_str(), ctx.svc_start_ms[i] / 1000.0);
            } else {
                snprintf(sb, sizeof(sb), "%-4s:%-4s", ctx.short_names[i], ctx.cur_states[i].c_str());
            }
            uint32_t sc_col = (ctx.cur_states[i] == "RUN") ? kColorAccent
                : ((ctx.cur_states[i] == "STOP") ? kColorMuted : kColorPrimary);
            draw_text(px, stride, sb, sx, sy, 2, sc_col);
        }

        draw_tail_block(px, stride, rx0, rx1, 184, 53, ctx);
    } else {
        draw_text(px, stride, bl_init_line.c_str(), 24, 362, 2, kColorMuted);

        fill_rect(px, stride, 24, 386, left_r, 388, kColorTrack);
        char svc1[72], svc2[72];
        snprintf(svc1, sizeof(svc1), "SF:%-4s ANIM:%-4s ZYG:%-4s APEX:%-4s ARTD:%-4s",
                 ctx.cur_states[0].c_str(), ctx.cur_states[1].c_str(), ctx.cur_states[2].c_str(),
                 ctx.cur_states[3].c_str(), ctx.cur_states[4].c_str());
        snprintf(svc2, sizeof(svc2), "WPA:%-4s VOLD:%-4s KST2:%-4s INST:%-4s SYS:%-4s",
                 ctx.cur_states[5].c_str(), ctx.cur_states[6].c_str(), ctx.cur_states[7].c_str(),
                 ctx.cur_states[8].c_str(), ctx.cur_states[9].c_str());
        draw_text(px, stride, svc1, 24, 396, 2, kColorPrimary);
        draw_text(px, stride, svc2, 24, 417, 2, kColorPrimary);

        draw_resources_block(px, stride, 24, left_r, 442, left_chars, ctx);

        std::string top_line = "TOP ";
        if (ctx.sys->top_cpu.empty()) {
            top_line += "SAMPLING /PROC...";
        } else {
            for (size_t i = 0; i < ctx.sys->top_cpu.size() && i < 3; i++) {
                char tb[32];
                std::string nm = ctx.sys->top_cpu[i].comm;
                if (nm.size() > 10) nm.resize(10);
                snprintf(tb, sizeof(tb), "%s %.0f%%", nm.c_str(), ctx.sys->top_cpu[i].pct);
                if (i > 0) top_line += "  ";
                top_line += tb;
            }
        }
        if (top_line.size() > left_chars) top_line.resize(left_chars);
        draw_text(px, stride, top_line.c_str(), 24, 536, 2, kColorPrimary);

        if (footer.size() > 40) footer.resize(40);
        draw_text(px, stride, footer.c_str(), 24, 560, 2, kColorMuted);
        draw_text(px, stride, ctx.rules_label.c_str(),
                  left_r - (int)ctx.rules_label.size() * 12, 560, 2, kColorMuted);

        draw_tail_block(px, stride, 24, left_r, 586, 59, ctx);
    }
}

}  // namespace

int run_display(const DisplayOptions& opts, int argc, char** argv) {
    bool preview = (opts.mode != DisplayMode::Early);
    bool replay_mode = (opts.mode == DisplayMode::Replay);
    bool replay_bl = opts.replay_bl;
    bool replay_pre_sf = opts.replay_pre_sf;
    float replay_speed = opts.replay_speed;
    if (replay_speed < 0.25f || replay_speed > 10.0f) replay_speed = 1.0f;

    if (!preview && is_display_disabled()) {
        return 0;
    }
    if (!preview) {
        reexec_from_tmp_if_needed(opts, argc, argv);
    }

    FILE* logf = preview ? stdout : fopen(kLogPath, "w");
    if (!logf) logf = stdout;
    setvbuf(logf, nullptr, _IOLBF, 0);

    if (!preview) {
        write_pid_file();
        int64_t start_ms = boottime_ms();
        fprintf(logf, "started_ms=%lld\n", (long long)start_ms);
        trace_open();
        trace_life(start_ms, "started");
    }

    EventState ev_state;
    load_rules(&ev_state.rules);
    DeviceInfo dev_info = collect_device_info(ev_state.rules);
    BootloaderProfile bl_profile = collect_bootloader_profile();
    std::string bootloader_line = bl_profile.summary_line;
    init_phases(&ev_state, bl_profile);
    if (!preview && !replay_mode) {
        trace_bl_profile(boottime_ms(), bl_profile);
    }

    ReplayData replay_data;
    if (replay_mode) {
        std::string resolved = resolve_run_dir(opts.replay_dir);
        if (resolved.empty() || !load_replay_data(resolved, &replay_data)) {
            fprintf(logf, "exit=replay_load_failed\n");
            return 1;
        }
        bootloader_line = replay_data.bl_line;
        const BootloaderProfile& bp = replay_data.bl_profile;
        fprintf(logf, "bl_source=%s bl_total_ms=%lld bl_stages=%zu bl_summary=%s\n",
                bp.source.empty() ? "NONE" : bp.source.c_str(),
                (long long)replay_data.bl_total_ms,
                bp.stages.size(),
                bootloader_line.c_str());
    } else {
        fprintf(logf, "bl_source=%s bl_total_ms=%lld bl_stages=%zu bl_summary=%s\n",
                bl_profile.source.empty() ? "NONE" : bl_profile.source.c_str(),
                (long long)bl_profile.total_ms,
                bl_profile.stages.size(),
                bootloader_line.c_str());
    }

    pthread_t ev_thread{};
    pthread_t kmsg_thread{};
    pthread_t err_thread{};
    bool ev_started = !replay_mode && (pthread_create(&ev_thread, nullptr, event_reader_thread, &ev_state) == 0);
    bool kmsg_started = !replay_mode && (pthread_create(&kmsg_thread, nullptr, kmsg_reader_thread, &ev_state) == 0);
    bool err_started = !replay_mode && (pthread_create(&err_thread, nullptr, err_reader_thread, &ev_state) == 0);
    auto stop_threads = [&]() {
        ev_state.stop = true;
        {
            std::lock_guard<std::mutex> lock(ev_state.mu);
            if (ev_state.logcat_pid > 0) kill(ev_state.logcat_pid, SIGKILL);
            if (ev_state.err_logcat_pid > 0) kill(ev_state.err_logcat_pid, SIGKILL);
        }
        if (ev_started) pthread_join(ev_thread, nullptr);
        if (kmsg_started) pthread_join(kmsg_thread, nullptr);
        if (err_started) pthread_join(err_thread, nullptr);
        ev_started = kmsg_started = err_started = false;
    };

    const char* watched[] = {
        "surfaceflinger", "bootanim", "zygote", "apexd", "artd",
        "wpa_supplicant", "vold", "keystore2", "installd", "system_server"
    };
    const char* short_names[] = {
        "SF", "ANIM", "ZYG", "APEX", "ARTD",
        "WPA", "VOLD", "KST2", "INST", "SYS"
    };
    constexpr int kSvcCount = 10;
    std::string prev_states[kSvcCount];
    int64_t svc_start_ms[kSvcCount];
    for (int i = 0; i < kSvcCount; i++) svc_start_ms[i] = -1;
    std::string last_transition = "WAITING TRANSITIONS";
    PsiSample prev_psi[3];
    int64_t prev_psi_ms = 0;
    SysSampleState sys_state;
    SysMetrics sys_metrics;

    LibGuiApi gui;
    if (!gui.load()) {
        fprintf(logf, "exit=libgui_load_failed\n");
        trace_life(boottime_ms(), "exit", "libgui_load_failed");
        stop_threads();
        trace_close();
        if (!preview) {
            unlink(kPidPath);
            unlink(kTmpExecPath);
        }
        return 1;
    }
    if (gui.ps_self && gui.ps_start) {
        Sp ps = gui.ps_self();
        if (ps.ptr) gui.ps_start(ps.ptr);
    }

    Sp client;
    Sp display_token;
    Sp sc;
    Sp surf;
    ANativeWindow* anw = nullptr;
    int64_t sf_deadline = boottime_ms() + 8000;
    int sf_wait_tick = 0;
    while (boottime_ms() < sf_deadline) {
        if (!preview && (get_prop("sys.boot_completed") == "1" || get_prop("service.bootanim.exit") == "1")) {
            fprintf(logf, "exit=boot_already_completed\n");
            trace_life(boottime_ms(), "exit", "boot_already_completed");
            stop_threads();
            trace_close();
            unlink(kPidPath);
            unlink(kTmpExecPath);
            return 0;
        }
        if (!preview && !replay_mode) {
            int64_t t_ms = boottime_ms();
            std::string pre_svcs[kSvcCount];
            bool pre_changed = false;
            for (int i = 0; i < 9; i++) {
                pre_svcs[i] = svc_state(watched[i]);
                if (pre_svcs[i] == "RUN" && svc_start_ms[i] < 0) svc_start_ms[i] = t_ms;
                if (prev_states[i] != pre_svcs[i]) pre_changed = true;
                prev_states[i] = pre_svcs[i];
            }
            pre_svcs[9] = "-";
            if (pre_svcs[2] == "RUN") {
                std::lock_guard<std::mutex> lock(ev_state.mu);
                advance_phases_for_zygote(ev_state.phases, t_ms);
            }
            if ((sf_wait_tick % 5) == 0 || pre_changed) {
                PsiSample cur_psi[3];
                read_psi_file("/proc/pressure/cpu", &cur_psi[0]);
                read_psi_file("/proc/pressure/io", &cur_psi[1]);
                read_psi_file("/proc/pressure/memory", &cur_psi[2]);
                int64_t dt_ms = prev_psi_ms > 0 ? (t_ms - prev_psi_ms) : 0;
                int c_pct = compute_psi_pct(cur_psi[0], prev_psi[0], dt_ms);
                int i_pct = compute_psi_pct(cur_psi[1], prev_psi[1], dt_ms);
                int m_pct = compute_psi_pct(cur_psi[2], prev_psi[2], dt_ms);
                for (int i = 0; i < 3; i++) prev_psi[i] = cur_psi[i];
                prev_psi_ms = t_ms;
                sys_metrics = collect_sys_metrics(t_ms, &sys_state);
                populate_resource_rows(&sys_metrics, c_pct, i_pct, m_pct);
                trace_metric(t_ms, c_pct, i_pct, m_pct, sys_metrics, pre_svcs, kSvcCount);
                for (const auto& r : sys_metrics.rows) trace_kv(t_ms, r.key, r.text, r.alert);
            }
        }
        sf_wait_tick++;

        client = gui.scc_default();
        if (client.ptr) {
            if (gui.get_phys_ids && gui.get_phys_token) {
                auto ids = gui.get_phys_ids();
                if (!ids.empty()) {
                    display_token = gui.get_phys_token(ids[0]);
                }
            }
            if (!display_token.ptr && gui.get_internal_token) {
                display_token = gui.get_internal_token();
            }
            if (display_token.ptr) {
                Geometry init_geom = query_geometry(gui, display_token);
                bool init_land = init_geom.width > init_geom.height;
                set_canvas_bounds(init_land ? kLandWidth : kPortraitWidth,
                                  init_land ? kLandHeight : kPortraitHeight);

                Blob name{}, metadata{};
                gui.str8_ctor(&name, kLayerName);
                gui.lm_ctor(&metadata);
                Sp null_parent{};
                uint32_t hint = 0;
                sc = gui.scc_create(
                    client.ptr, &name, canvas_width(), canvas_height(), 1 /*RGBA_8888*/,
                    0x00000400 /*eOpaque*/, null_parent, &metadata, &hint);
                gui.str8_dtor(&name);
                if (sc.ptr) {
                    surf = gui.sc_get_surface(sc.ptr);
                    if (surf.ptr) {
                        anw = extract_anw(surf.ptr);
                        if (anw) {
                            trace_life(boottime_ms(), "sf_ready");
                            break;
                        }
                    }
                }
            }
        }
        usleep(100000);
    }

    if (!anw) {
        fprintf(logf, "exit=surface_unavailable client=%p token=%p sc=%p surf=%p\n",
                client.ptr, display_token.ptr, sc.ptr, surf.ptr);
        trace_life(boottime_ms(), "exit", "surface_unavailable");
        stop_threads();
        trace_close();
        if (!preview) {
            unlink(kPidPath);
            unlink(kTmpExecPath);
        }
        return 1;
    }

    Geometry last_geom;
    bool has_geom = false;
    bool first = true;
    int frame_idx = 0;
    PhaseNode prev_traced_phases[6];
    bool has_traced_phases = false;
    StageInfo prev_traced_stage;
    bool has_traced_stage = false;

    int64_t loop_start = boottime_ms();
    bool saw_bootanim = false;
    int64_t deadline = loop_start + (preview ? 4000 : 30000);
    bool replay_finished = false;
    while (replay_mode ? !replay_finished : (boottime_ms() < deadline)) {
        if (!preview) {
            if (get_prop("init.svc.bootanim") == "running") saw_bootanim = true;
            if (get_prop("service.bootanim.exit") == "1" || get_prop("sys.boot_completed") == "1") {
                fprintf(logf, "exit=animation_ended\n");
                trace_life(boottime_ms(), "exit", "animation_ended");
                break;
            }
            if ((saw_bootanim || boottime_ms() - loop_start > 4000) && !animation_active()) {
                fprintf(logf, "exit=animation_ended\n");
                trace_life(boottime_ms(), "exit", "animation_ended");
                break;
            }
        }

        Geometry geom = query_geometry(gui, display_token);
        bool geom_changed = !has_geom || !geom.same_transform(last_geom);
        if (geom_changed) {
            Blob tx{};
            gui.tx_ctor(&tx);
            if (first) gui.tx_set_layer(&tx, sc, kLayer);
            gui.tx_set_pos(&tx, sc, geom.pos_x, geom.pos_y);
            gui.tx_set_matrix(&tx, sc, geom.dsdx, geom.dtdx, geom.dtdy, geom.dsdy);
            if (first) gui.tx_show(&tx, sc);
            gui.tx_apply(&tx, false, false);

            char lbuf[96];
            snprintf(lbuf, sizeof(lbuf), "%dx%d@%d rot=%d pos=%d,%d scale=%.3f",
                     geom.width, geom.height, geom.refresh_hz, geom.rot_quad,
                     (int)lroundf(geom.pos_x), (int)lroundf(geom.pos_y), geom.scale);
            fprintf(logf, "%s%s\n", has_geom ? "layout_changed=" : "layout=", lbuf);
            trace_life(boottime_ms(), has_geom ? "layout_changed" : "layout", lbuf);
            last_geom = geom;
            has_geom = true;
        }

        int64_t now_ms = boottime_ms();
        bool in_bl_phase = false;
        int64_t bl_por_ms = 0;
        std::string bl_exp_line1;
        std::string bl_exp_line2;
        if (replay_mode) {
            int64_t bl_span_ms = (replay_bl && replay_data.bl_profile.timeline_ms > 0)
                ? replay_data.bl_profile.timeline_ms
                : 0;
            int64_t elapsed_ms = now_ms - loop_start;
            if (elapsed_ms < 0) elapsed_ms = 0;
            int64_t virtual_elapsed = (int64_t)llround((double)elapsed_ms * (double)replay_speed);
            if (bl_span_ms > 0 && virtual_elapsed < bl_span_ms) {
                in_bl_phase = true;
                bl_por_ms = virtual_elapsed;
                now_ms = 0;
            } else {
                int64_t post_bl_elapsed = virtual_elapsed - bl_span_ms;
                int64_t base_ms = replay_pre_sf ? replay_data.pre_start_ms : replay_data.start_ms;
                int64_t sim_now = base_ms + post_bl_elapsed;
                if (sim_now >= replay_data.end_ms) {
                    now_ms = replay_data.end_ms;
                    replay_finished = true;
                } else {
                    now_ms = sim_now;
                }
            }
        }
        double uptime_s = now_ms / 1000.0;
        std::string cur_states[kSvcCount];
        int cpu_pct = -1, io_pct = -1, mem_pct = -1;
        StageInfo stage;
        PhaseNode phases_snap[6];
        std::string wait_svc;
        int64_t wait_dur_ms = 0;
        std::string max_init_svc;
        int64_t max_init_dur_ms = 0;
        std::string last_action;
        std::vector<std::pair<std::string, int64_t>> init_ms_snap;
        size_t active_cnt = 0;
        std::vector<TailEntry> visible_tail;
        size_t max_rows = (canvas_width() == kLandWidth) ? kMaxTailRows : 11;

        bool svc_changed = false;
        if (replay_mode) {
            std::vector<TailEntry> full_tail;
            if (in_bl_phase) {
                build_bl_replay_snapshot(
                    replay_data, bl_por_ms, ev_state.rules, kSvcCount,
                    phases_snap, &stage, &bl_exp_line1, &bl_exp_line2,
                    cur_states, svc_start_ms, &sys_metrics, &full_tail, &last_transition);
            } else {
                build_replay_snapshot(
                    replay_data, now_ms, ev_state.rules, short_names, kSvcCount,
                    phases_snap, &stage, &init_ms_snap, &last_action,
                    &max_init_svc, &max_init_dur_ms, &wait_svc, &wait_dur_ms, &active_cnt,
                    cur_states, svc_start_ms, &cpu_pct, &io_pct, &mem_pct,
                    &sys_metrics, &full_tail, &last_transition);
            }
            size_t total = full_tail.size();
            size_t start_idx = total > max_rows ? (total - max_rows) : 0;
            for (size_t i = start_idx; i < total; i++) {
                visible_tail.push_back(std::move(full_tail[i]));
            }
        } else {
            for (int i = 0; i < 9; i++) {
                cur_states[i] = svc_state(watched[i]);
                if (cur_states[i] == "RUN" && svc_start_ms[i] < 0) svc_start_ms[i] = now_ms;
                if (!first && prev_states[i] != cur_states[i]) {
                    svc_changed = true;
                    char tbuf[64];
                    snprintf(tbuf, sizeof(tbuf), "+%.2fS %s %s>%s",
                             uptime_s, short_names[i], prev_states[i].c_str(), cur_states[i].c_str());
                    last_transition = tbuf;
                    push_tail(&ev_state, now_ms, "S", TailKind::Stage,
                              std::string(short_names[i]) + " " + prev_states[i] + ">" + cur_states[i]);
                }
                prev_states[i] = cur_states[i];
            }

            PsiSample cur_psi[3];
            read_psi_file("/proc/pressure/cpu", &cur_psi[0]);
            read_psi_file("/proc/pressure/io", &cur_psi[1]);
            read_psi_file("/proc/pressure/memory", &cur_psi[2]);
            int64_t dt_ms = prev_psi_ms > 0 ? (now_ms - prev_psi_ms) : 0;
            cpu_pct = compute_psi_pct(cur_psi[0], prev_psi[0], dt_ms);
            io_pct = compute_psi_pct(cur_psi[1], prev_psi[1], dt_ms);
            mem_pct = compute_psi_pct(cur_psi[2], prev_psi[2], dt_ms);
            for (int i = 0; i < 3; i++) prev_psi[i] = cur_psi[i];
            prev_psi_ms = now_ms;
            if (first && !cur_psi[0].valid) {
                push_tail(&ev_state, now_ms, "M", TailKind::ModuleError, "ERR PSI UNAVAIL", "MOD:PSI");
            }
            if ((frame_idx % 2) == 0) {
                sys_metrics = collect_sys_metrics(now_ms, &sys_state);
            }
            populate_resource_rows(&sys_metrics, cpu_pct, io_pct, mem_pct);

            {
                std::lock_guard<std::mutex> lock(ev_state.mu);
                if (cur_states[2] == "RUN") advance_phases_for_zygote(ev_state.phases, now_ms);
                if (!ev_state.last_init_action.empty() || !ev_state.max_init_svc.empty()) {
                    std::string init_det;
                    if (!ev_state.last_init_action.empty()) {
                        init_det = "ACT:" + short_init_action(ev_state.last_init_action);
                    }
                    if (!ev_state.max_init_svc.empty()) {
                        char mbuf[32];
                        snprintf(mbuf, sizeof(mbuf), "MAX:%s %.2fS",
                                 ev_state.max_init_svc.c_str(), ev_state.max_init_dur_ms / 1000.0);
                        if (!init_det.empty()) init_det += " ";
                        init_det += mbuf;
                    }
                    ev_state.phases[1].detail = init_det;
                }
                for (int i = 0; i < 6; i++) phases_snap[i] = ev_state.phases[i];
                for (const auto& a : ev_state.active_svcs) {
                    int64_t d = now_ms - a.start_ms;
                    if (d > wait_dur_ms) {
                        wait_dur_ms = d;
                        wait_svc = a.name;
                    }
                }
                active_cnt = ev_state.active_svcs.size();
                last_action = ev_state.last_init_action;
                max_init_svc = ev_state.max_init_svc;
                max_init_dur_ms = ev_state.max_init_dur_ms;
                init_ms_snap = ev_state.init_milestones;
                stage = ev_state.framework_stage;
                size_t total = ev_state.tail.size();
                size_t start_idx = total > max_rows ? (total - max_rows) : 0;
                for (size_t i = start_idx; i < total; i++) {
                    visible_tail.push_back(ev_state.tail[i]);
                }
            }
            if (stage.step == 0) {
                if (get_prop("sys.boot_completed") == "1" || get_prop("service.bootanim.exit") == "1") {
                    stage = {10, "BOOT_COMPLETED", ""};
                    for (int i = 2; i < 6; i++) {
                        if (phases_snap[i].state != NodeState::Done) {
                            phases_snap[i].state = NodeState::Done;
                            phases_snap[i].start_ms = -1;
                            phases_snap[i].end_ms = -1;
                        }
                    }
                } else {
                    stage = infer_native_stage(cur_states[3], cur_states[2]);
                }
            }
            cur_states[9] = (stage.step >= 4) ? "RUN" : "-";
            if (cur_states[9] == "RUN" && svc_start_ms[9] < 0) svc_start_ms[9] = now_ms;
            prev_states[9] = cur_states[9];
        }

        int expanded_idx = -1;
        for (int i = 5; i >= 0; i--) {
            if (phases_snap[i].state == NodeState::Running) {
                expanded_idx = i;
                break;
            }
        }

        auto phase_dur_str = [&](int idx) -> std::string {
            const PhaseNode& p = phases_snap[idx];
            if (p.start_ms < 0) return "--";
            int64_t end = (p.state == NodeState::Done && p.end_ms >= p.start_ms) ? p.end_ms : now_ms;
            if (end < p.start_ms) return "--";
            char b[16];
            snprintf(b, sizeof(b), "%.2fS", (end - p.start_ms) / 1000.0);
            return b;
        };

        std::string exp_line1;
        std::string exp_line2;
        uint32_t exp_col2 = kColorMuted;
        std::string psi_compact;
        if (cpu_pct >= 0 && io_pct >= 0 && mem_pct >= 0) {
            char pb[32];
            snprintf(pb, sizeof(pb), "PSI C:%2d%% I:%2d%% M:%2d%%", cpu_pct, io_pct, mem_pct);
            psi_compact = pb;
        }
        if (expanded_idx == 1) {
            std::string act = last_action.empty() ? "EARLY-INIT" : short_init_action(last_action);
            char b1[96];
            if (!max_init_svc.empty()) {
                snprintf(b1, sizeof(b1), "ACT:%s  SVC:%zu  MAX:%s %.2fS",
                         act.c_str(), active_cnt, max_init_svc.c_str(), max_init_dur_ms / 1000.0);
            } else {
                snprintf(b1, sizeof(b1), "ACT:%s  ACTIVE_SVC:%zu", act.c_str(), active_cnt);
            }
            exp_line1 = sanitize_tail_text(ev_state.rules, b1, 57);
            if (!wait_svc.empty() && wait_dur_ms >= 200) {
                char b2[64];
                snprintf(b2, sizeof(b2), "WAIT:%s %.1fS", wait_svc.c_str(), wait_dur_ms / 1000.0);
                exp_line2 = sanitize_tail_text(ev_state.rules, b2, 34);
                exp_col2 = (wait_dur_ms >= 3000) ? kColorError : kColorPrimary;
            } else {
                exp_line2 = "WAIT:IDLE";
                exp_col2 = kColorMuted;
            }
        } else if (expanded_idx == 2) {
            std::string apex_st = get_prop("apexd.status");
            if (apex_st.empty()) apex_st = cur_states[3];
            exp_line1 = sanitize_tail_text(
                ev_state.rules,
                "ZYG:" + cur_states[2] + "  APEX:" + apex_st + "  ARTD:" + cur_states[4],
                57);
            if (!wait_svc.empty() && wait_dur_ms >= 200) {
                char b2[64];
                snprintf(b2, sizeof(b2), "WAIT:%s %.1fS", wait_svc.c_str(), wait_dur_ms / 1000.0);
                exp_line2 = sanitize_tail_text(ev_state.rules, b2, 34);
                exp_col2 = (wait_dur_ms >= 3000) ? kColorError : kColorPrimary;
            } else {
                exp_line2 = "WAIT:BOOT_PROGRESS_START";
                exp_col2 = kColorPrimary;
            }
        } else if (expanded_idx == 3) {
            exp_line1 = sanitize_tail_text(
                ev_state.rules,
                "STEP:" + stage.label + "  INIT:" + phase_dur_str(1) + "  ZYG:" + phase_dur_str(2),
                57);
            exp_line2 = "PRELOADING FRAMEWORK CLASSES";
            exp_col2 = kColorPrimary;
        } else if (expanded_idx == 4) {
            exp_line1 = sanitize_tail_text(
                ev_state.rules,
                "STEP:" + stage.label + "  ARTD:" + cur_states[4] + "  PRE:" + phase_dur_str(3),
                57);
            exp_line2 = (io_pct >= 30) ? "PKG SCAN IO BOUND" : "PKG/DEX VERIFY SCAN";
            exp_col2 = (io_pct >= 30 || cpu_pct >= 65) ? kColorError : kColorPrimary;
        } else if (expanded_idx == 5) {
            exp_line1 = sanitize_tail_text(
                ev_state.rules,
                "STEP:" + stage.label + "  PMS:" + phase_dur_str(4) + "  WPA:" + cur_states[5],
                57);
            std::string bc = get_prop("sys.boot_completed");
            std::string ae = get_prop("service.bootanim.exit");
            exp_line2 = "BC=" + (bc.empty() ? std::string("0") : bc)
                + " ANIM_EXIT=" + (ae.empty() ? std::string("0") : ae);
            exp_col2 = kColorPrimary;
        } else if (expanded_idx == 0) {
            if (!bl_exp_line1.empty()) {
                exp_line1 = bl_exp_line1;
                exp_line2 = bl_exp_line2;
                exp_col2 = kColorPrimary;
            } else {
                exp_line1 = sanitize_tail_text(ev_state.rules, bootloader_line, 57);
                exp_line2 = "WAITING EARLY INIT";
            }
        }

        std::string footer;
        if (last_transition == "WAITING TRANSITIONS") {
            std::string bc = get_prop("sys.boot_completed");
            std::string ae = get_prop("service.bootanim.exit");
            footer = "BC=" + (bc.empty() ? std::string("0") : bc)
                + " ANIM_EXIT=" + (ae.empty() ? std::string("0") : ae);
        } else {
            footer = "LAST " + last_transition;
        }
        char rbuf[24];
        snprintf(rbuf, sizeof(rbuf), "RULES:%s/%zu", ev_state.rules.source.c_str(), ev_state.rules.count);

        RenderContext ctx;
        ctx.preview = preview;
        ctx.replay = replay_mode;
        ctx.replay_bl_phase = replay_mode && in_bl_phase;
        ctx.replay_pre_sf_phase = replay_mode && !in_bl_phase && replay_pre_sf && (now_ms < replay_data.start_ms);
        ctx.bl_por_ms = bl_por_ms;
        ctx.frame_idx = frame_idx;
        ctx.now_ms = now_ms;
        ctx.uptime_s = uptime_s;
        ctx.geom = &geom;
        ctx.dev_info = &dev_info;
        ctx.stage = &stage;
        ctx.phases = phases_snap;
        ctx.expanded_idx = expanded_idx;
        ctx.exp_line1 = exp_line1;
        ctx.exp_line2 = exp_line2;
        ctx.exp_col2 = exp_col2;
        ctx.max_init_svc = max_init_svc;
        ctx.max_init_dur_ms = max_init_dur_ms;
        ctx.psi_compact = psi_compact;
        ctx.cpu_pct = cpu_pct;
        ctx.io_pct = io_pct;
        ctx.sys = &sys_metrics;
        ctx.cur_states = cur_states;
        ctx.svc_start_ms = svc_start_ms;
        ctx.short_names = short_names;
        ctx.svc_count = kSvcCount;
        ctx.init_ms = &init_ms_snap;
        ctx.bootloader_line = bootloader_line;
        ctx.footer = footer;
        ctx.rules_label = rbuf;
        ctx.rules = &ev_state.rules;
        ctx.tail = &visible_tail;
        ctx.max_rows = max_rows;

        ANativeWindow_Buffer buf{};
        if (ANativeWindow_lock(anw, &buf, nullptr) == 0 && buf.bits) {
            draw_dashboard((uint32_t*)buf.bits, buf.stride, ctx);
            ANativeWindow_unlockAndPost(anw);
        }

        if (!preview && !replay_mode) {
            if (svc_changed || (frame_idx % 2) == 0) {
                trace_metric(now_ms, cpu_pct, io_pct, mem_pct, sys_metrics, cur_states, kSvcCount);
            }
            for (const auto& r : sys_metrics.rows) trace_kv(now_ms, r.key, r.text, r.alert);
            for (int i = 0; i < 6; i++) {
                bool changed = !has_traced_phases
                    || prev_traced_phases[i].state != phases_snap[i].state
                    || prev_traced_phases[i].start_ms != phases_snap[i].start_ms
                    || prev_traced_phases[i].end_ms != phases_snap[i].end_ms
                    || prev_traced_phases[i].detail != phases_snap[i].detail;
                if (changed) {
                    trace_phase(now_ms, i, (int)phases_snap[i].state,
                                phases_snap[i].start_ms, phases_snap[i].end_ms,
                                phases_snap[i].detail);
                    prev_traced_phases[i] = phases_snap[i];
                }
            }
            has_traced_phases = true;
            if (!has_traced_stage || prev_traced_stage.step != stage.step
                    || prev_traced_stage.label != stage.label
                    || prev_traced_stage.raw_ms != stage.raw_ms) {
                trace_stage(now_ms, stage.step, stage.label, stage.raw_ms);
                prev_traced_stage = stage;
                has_traced_stage = true;
            }
        }

        if (first) {
            int64_t ff_ms = boottime_ms();
            fprintf(logf, "first_frame_submitted_ms=%lld\n", (long long)ff_ms);
            trace_life(ff_ms, "first_frame");
            first = false;
        }
        frame_idx++;
        if (!(replay_mode && replay_finished)) {
            usleep(250000);
        }
    }

    if (replay_mode) {
        usleep(1000000);
        fprintf(logf, "exit=replay_done\n");
    } else if (boottime_ms() >= deadline) {
        fprintf(logf, "exit=time_limit\n");
        trace_life(boottime_ms(), "exit", "time_limit");
    }

    stop_threads();

    if (gui.tx_hide && sc.ptr) {
        Blob tx{};
        gui.tx_ctor(&tx);
        gui.tx_hide(&tx, sc);
        gui.tx_apply(&tx, true, false);
    }
    int64_t rel_ms = boottime_ms();
    fprintf(logf, "released_ms=%lld\n", (long long)rel_ms);
    trace_life(rel_ms, "released");
    trace_close();
    if (!preview) {
        unlink(kPidPath);
        unlink(kTmpExecPath);
        if (logf != stdout) fclose(logf);
    }
    return 0;
}

}  // namespace bootscope
