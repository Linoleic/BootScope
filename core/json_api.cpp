#include "json_api.h"

#include "boot_sources.h"
#include "bootloader.h"
#include "collector.h"
#include "common.h"
#include "replay_source.h"
#include "rules.h"
#include "sys_metrics.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bootscope {
namespace {

const char* const kSvcShortNames[10] = {
    "SF", "ANIM", "ZYG", "APEX", "ARTD", "WPA", "VOLD", "KST2", "INST", "SYS"
};

std::map<std::string, std::string> read_kv_map(const std::string& path) {
    std::map<std::string, std::string> m;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return m;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim_copy(line);
        size_t eq = s.find('=');
        if (eq != std::string::npos && eq > 0) {
            m[trim_copy(s.substr(0, eq))] = trim_copy(s.substr(eq + 1));
        }
    }
    fclose(f);
    return m;
}

std::string map_get(const std::map<std::string, std::string>& m, const char* key, const char* def = "") {
    auto it = m.find(key);
    return (it != m.end()) ? it->second : std::string(def);
}

int64_t map_get_i64(const std::map<std::string, std::string>& m, const char* key, int64_t def = -1) {
    auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return def;
    char* endp = nullptr;
    long long v = strtoll(it->second.c_str(), &endp, 10);
    if (endp == it->second.c_str() || *endp != '\0') return def;
    return (int64_t)v;
}

double map_get_dbl(const std::map<std::string, std::string>& m, const char* key, double def = -1.0) {
    auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return def;
    char* endp = nullptr;
    double v = strtod(it->second.c_str(), &endp);
    if (endp == it->second.c_str() || !std::isfinite(v)) return def;
    return v;
}

uint64_t calc_dir_bytes(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return 0;
    uint64_t total = 0;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        std::string child = path + "/" + ent->d_name;
        struct stat st {};
        if (lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            total += calc_dir_bytes(child);
        } else {
            total += (uint64_t)st.st_blocks * 512ULL;
        }
    }
    closedir(dir);
    return total;
}

const char* node_state_str(NodeState st) {
    switch (st) {
        case NodeState::Pending: return "pending";
        case NodeState::Running: return "running";
        case NodeState::Done: return "done";
    }
    return "pending";
}

struct InitSvcTiming {
    std::string name;
    int64_t ts_ms = 0;
    int64_t dur_ms = 0;
    std::string detail;
};

std::vector<InitSvcTiming> extract_init_service_timings(const ReplayData& rd) {
    std::vector<InitSvcTiming> out;
    for (const auto& ev : rd.init_events) {
        if (ev.kind != "exit") continue;
        const char* tp = strstr(ev.detail.c_str(), "took=");
        if (!tp) continue;
        double took_s = atof(tp + 5);
        int64_t dur_ms = (int64_t)llround(took_s * 1000.0);
        out.push_back({ev.name, ev.ts_ms, dur_ms, ev.detail});
    }
    std::sort(out.begin(), out.end(), [](const InitSvcTiming& a, const InitSvcTiming& b) {
        if (a.dur_ms != b.dur_ms) return a.dur_ms > b.dur_ms;
        return a.ts_ms < b.ts_ms;
    });
    return out;
}

int64_t infer_boot_done_ms(const ReplayData& rd, const std::map<std::string, std::string>& meta) {
    int64_t best = -1;
    for (const auto& bev : rd.boot_events) {
        if (bev.tag == "wm_boot_animation_done" || bev.tag == "sf_stop_bootanim"
                || bev.tag == "boot_progress_enable_screen") {
            if (bev.ts_ms > best) best = bev.ts_ms;
        }
    }
    if (best < 0 && !rd.boot_events.empty()) best = rd.boot_events.back().ts_ms;
    if (best < 0) {
        double fu = map_get_dbl(meta, "finished_uptime", -1.0);
        if (fu > 0.0) best = (int64_t)llround(fu * 1000.0);
    }
    return best;
}

struct RunSnapshotBundle {
    std::string dir;
    std::string boot_id;
    time_t mtime = 0;
    std::map<std::string, std::string> meta;
    std::map<std::string, std::string> dstat;
    std::map<std::string, std::string> drend;
    ReplayData rd;
    PhaseNode phases[6];
    StageInfo stage;
    std::vector<std::pair<std::string, int64_t>> init_ms;
    std::string last_act;
    std::string max_init_svc;
    int64_t max_init_dur_ms = 0;
    std::string wait_svc;
    int64_t wait_dur_ms = 0;
    size_t active_cnt = 0;
    std::string cur_states[10];
    int64_t svc_start_ms[10]{};
    int cpu_pct = -1;
    int io_pct = -1;
    int mem_pct = -1;
    SysMetrics sys;
    std::vector<TailEntry> tails;
    std::string last_transition;
    int64_t boot_done_ms = -1;
};

bool load_bundle(const std::string& target, const RuleSet& rules, RunSnapshotBundle* out) {
    std::string dir = resolve_run_dir(target);
    if (dir.empty()) return false;
    struct stat st {};
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (!load_replay_data(dir, &out->rd)) return false;

    out->dir = dir;
    out->mtime = st.st_mtim.tv_sec;
    out->meta = read_kv_map(dir + "/metadata.txt");
    out->dstat = read_kv_map(dir + "/display-status.txt");
    out->drend = read_kv_map(dir + "/display-renderer.txt");
    out->boot_id = map_get(out->meta, "boot_id", "");
    if (out->boot_id.empty()) {
        size_t sl = dir.rfind('/');
        out->boot_id = (sl == std::string::npos) ? dir : dir.substr(sl + 1);
    }
    for (int i = 0; i < 10; i++) out->svc_start_ms[i] = -1;
    int64_t snap_ms = std::max<int64_t>(out->rd.end_ms, 60000);
    build_replay_snapshot(
        out->rd, snap_ms, rules, kSvcShortNames, 10,
        out->phases, &out->stage, &out->init_ms, &out->last_act,
        &out->max_init_svc, &out->max_init_dur_ms, &out->wait_svc, &out->wait_dur_ms,
        &out->active_cnt, out->cur_states, out->svc_start_ms,
        &out->cpu_pct, &out->io_pct, &out->mem_pct,
        &out->sys, &out->tails, &out->last_transition);
    out->boot_done_ms = infer_boot_done_ms(out->rd, out->meta);
    return true;
}

int64_t phase_dur(const PhaseNode& p) {
    if (p.start_ms >= 0 && p.end_ms >= p.start_ms) return p.end_ms - p.start_ms;
    return -1;
}

void print_config_fields() {
    bool disp_en = !is_display_disabled();
    int max_hist = read_config_int("max_history", 20);
    bool ksu_early = (access(kEarlyBinPath, X_OK) == 0);
    printf("{\"display_enabled\":%s,\"max_history\":%d,\"ksu_early_installed\":%s}",
           disp_en ? "true" : "false", max_hist, ksu_early ? "true" : "false");
}

int64_t count_trace_errors(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    int64_t cnt = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "\tTAIL\t") != nullptr &&
                (strstr(line, "\tERR\t") != nullptr || strstr(line, "\tMERR\t") != nullptr)) {
            cnt++;
        }
    }
    fclose(f);
    return cnt;
}

}  // namespace

int cmd_list_json(int limit) {
    if (limit <= 0) limit = 30;
    auto runs = list_run_dirs_sorted();
    uint64_t total_bytes = calc_dir_bytes(kBaseDir);
    long disk_kb = (long)(total_bytes / 1024ULL);

    std::string model = get_prop("ro.product.model");
    std::string dev = get_prop("ro.product.device");
    std::string rel = get_prop("ro.build.version.release");
    std::string sdk = get_prop("ro.build.version.sdk");
    std::string abi = get_prop("ro.product.cpu.abi");
    std::string slot = get_prop("ro.boot.slot_suffix");

    printf("{\"device\":{\"model\":\"%s\",\"device\":\"%s\",\"android_version\":\"%s\","
           "\"sdk\":\"%s\",\"abi\":\"%s\",\"slot\":\"%s\"},",
           json_escape(model).c_str(), json_escape(dev).c_str(), json_escape(rel).c_str(),
           json_escape(sdk).c_str(), json_escape(abi).c_str(), json_escape(slot).c_str());
    printf("\"config\":");
    print_config_fields();
    printf(",\"total_runs\":%zu,\"disk_kb\":%ld,\"runs\":[", runs.size(), disk_kb);

    size_t shown = std::min<size_t>(runs.size(), (size_t)limit);
    for (size_t i = 0; i < shown; i++) {
        const auto& r = runs[i];
        auto meta = read_kv_map(r.path + "/metadata.txt");
        auto dstat = read_kv_map(r.path + "/display-status.txt");
        auto drend = read_kv_map(r.path + "/display-renderer.txt");
        ReplayData rd;
        load_replay_data(r.path, &rd);
        int64_t done_ms = infer_boot_done_ms(rd, meta);
        int64_t ff_ms = map_get_i64(drend, "first_frame_submitted_ms", -1);
        int64_t rel_ms = map_get_i64(drend, "released_ms", -1);
        double fin_s = map_get_dbl(meta, "finished_uptime", -1.0);
        int64_t err_cnt = count_trace_errors(r.path + "/early-trace.tsv");

        if (i > 0) putchar(',');
        printf("{\"boot_id\":\"%s\",\"mtime_epoch\":%lld,\"android_version\":\"%s\",\"sdk\":\"%s\","
               "\"mode\":\"%s\",\"bl_source\":\"%s\",\"bl_total_ms\":%lld,\"bl_wait_ms\":%lld,"
               "\"first_frame_ms\":%lld,\"released_ms\":%lld,\"boot_done_ms\":%lld,"
               "\"finished_uptime_s\":%.2f,\"boot_completed\":\"%s\",\"error_count\":%lld}",
               json_escape(r.boot_id).c_str(), (long long)r.mtime,
               json_escape(map_get(meta, "android_version")).c_str(),
               json_escape(map_get(meta, "sdk")).c_str(),
               json_escape(map_get(dstat, "mode", "probe")).c_str(),
               json_escape(rd.bl_profile.source.empty() ? "NONE" : rd.bl_profile.source).c_str(),
               (long long)rd.bl_total_ms, (long long)rd.bl_profile.wait_ms,
               (long long)ff_ms, (long long)rel_ms, (long long)done_ms,
               fin_s, json_escape(map_get(meta, "boot_completed")).c_str(),
               (long long)err_cnt);
    }
    printf("]}\n");
    return 0;
}

int cmd_inspect_json(const std::string& target) {
    RuleSet rules;
    load_rules(&rules);
    RunSnapshotBundle b;
    if (!load_bundle(target, rules, &b)) {
        printf("{\"ok\":false,\"error\":\"run_not_found\"}\n");
        return 1;
    }

    printf("{\"ok\":true,\"boot_id\":\"%s\",\"mtime_epoch\":%lld,",
           json_escape(b.boot_id).c_str(), (long long)b.mtime);

    printf("\"metadata\":{");
    bool first_kv = true;
    for (const auto& kv : b.meta) {
        if (!first_kv) putchar(',');
        first_kv = false;
        printf("\"%s\":\"%s\"", json_escape(kv.first).c_str(), json_escape(kv.second).c_str());
    }
    printf("},");

    printf("\"display\":{\"mode\":\"%s\",\"started_ms\":%lld,\"first_frame_ms\":%lld,"
           "\"released_ms\":%lld,\"layout\":\"%s\",\"exit\":\"%s\"},",
           json_escape(map_get(b.dstat, "mode", "probe")).c_str(),
           (long long)map_get_i64(b.drend, "started_ms", -1),
           (long long)map_get_i64(b.drend, "first_frame_submitted_ms", -1),
           (long long)map_get_i64(b.drend, "released_ms", -1),
           json_escape(map_get(b.drend, "layout")).c_str(),
           json_escape(map_get(b.drend, "exit")).c_str());

    const BootloaderProfile& bp = b.rd.bl_profile;
    printf("\"bootloader\":{\"source\":\"%s\",\"summary\":\"%s\",\"meta\":\"%s\","
           "\"total_ms\":%lld,\"timeline_ms\":%lld,\"wait_ms\":%lld,\"stages\":[",
           json_escape(bp.source.empty() ? "NONE" : bp.source).c_str(),
           json_escape(b.rd.bl_line).c_str(),
           json_escape(bp.meta_line).c_str(),
           (long long)b.rd.bl_total_ms,
           (long long)bp.timeline_ms,
           (long long)bp.wait_ms);
    for (size_t i = 0; i < bp.stages.size(); i++) {
        const auto& s = bp.stages[i];
        if (i > 0) putchar(',');
        printf("{\"id\":\"%s\",\"start_ms\":%lld,\"dur_ms\":%lld,\"wait_only\":%s,\"detail\":\"%s\"}",
               json_escape(s.id).c_str(), (long long)s.start_ms, (long long)s.dur_ms,
               s.wait_only ? "true" : "false", json_escape(s.detail).c_str());
    }
    printf("],\"events\":[");
    for (size_t i = 0; i < bp.events.size(); i++) {
        const auto& ev = bp.events[i];
        if (i > 0) putchar(',');
        printf("{\"por_ms\":%lld,\"major\":%s,\"text\":\"%s\"}",
               (long long)ev.por_ms, ev.major ? "true" : "false", json_escape(ev.text).c_str());
    }
    printf("]},");

    printf("\"phases\":[");
    for (int i = 0; i < 6; i++) {
        const auto& p = b.phases[i];
        if (i > 0) putchar(',');
        printf("{\"id\":\"%s\",\"state\":\"%s\",\"start_ms\":%lld,\"end_ms\":%lld,\"dur_ms\":%lld,\"detail\":\"%s\"}",
               json_escape(p.id).c_str(), node_state_str(p.state),
               (long long)p.start_ms, (long long)p.end_ms, (long long)phase_dur(p),
               json_escape(p.detail).c_str());
    }
    printf("],");

    std::vector<std::pair<std::string, int64_t>> all_init_phases;
    for (const auto& ev : b.rd.init_events) {
        if (ev.kind != "phase") continue;
        bool dup = false;
        for (const auto& ex : all_init_phases) {
            if (ex.first == ev.name) { dup = true; break; }
        }
        if (!dup && all_init_phases.size() < 24) {
            all_init_phases.emplace_back(ev.name, ev.ts_ms);
        }
    }
    printf("\"init_milestones\":[");
    for (size_t i = 0; i < all_init_phases.size(); i++) {
        if (i > 0) putchar(',');
        printf("{\"name\":\"%s\",\"ts_ms\":%lld}",
               json_escape(all_init_phases[i].first).c_str(), (long long)all_init_phases[i].second);
    }
    printf("],");

    auto svcs = extract_init_service_timings(b.rd);
    printf("\"top_init_services\":[");
    for (size_t i = 0; i < svcs.size() && i < 15; i++) {
        if (i > 0) putchar(',');
        printf("{\"name\":\"%s\",\"ts_ms\":%lld,\"dur_ms\":%lld,\"detail\":\"%s\"}",
               json_escape(svcs[i].name).c_str(), (long long)svcs[i].ts_ms,
               (long long)svcs[i].dur_ms, json_escape(svcs[i].detail).c_str());
    }
    printf("],");

    printf("\"boot_events\":[");
    for (size_t i = 0; i < b.rd.boot_events.size(); i++) {
        if (i > 0) putchar(',');
        printf("{\"tag\":\"%s\",\"ms\":%lld}",
               json_escape(b.rd.boot_events[i].tag).c_str(), (long long)b.rd.boot_events[i].ts_ms);
    }
    printf("],");

    struct PsiOut { int64_t ts_ms; int cpu; int io; int mem; };
    std::vector<PsiOut> psi_out;
    for (size_t i = 0; i < b.rd.psi_points.size(); i++) {
        const auto& pt = b.rd.psi_points[i];
        int c = (int)lround(pt.cpu_a), io_v = (int)lround(pt.io_a), m = (int)lround(pt.mem_a);
        if (i > 0) {
            int64_t dt = pt.ts_ms - b.rd.psi_points[i - 1].ts_ms;
            if (dt > 0) {
                auto pct = [&](uint64_t cur, uint64_t prev) {
                    if (cur < prev) return 0;
                    int v = (int)lround(((double)(cur - prev) / ((double)dt * 1000.0)) * 100.0);
                    return std::min(100, std::max(0, v));
                };
                c = pct(pt.cpu_t, b.rd.psi_points[i - 1].cpu_t);
                io_v = pct(pt.io_t, b.rd.psi_points[i - 1].io_t);
                m = pct(pt.mem_t, b.rd.psi_points[i - 1].mem_t);
            }
        }
        psi_out.push_back({pt.ts_ms, c, io_v, m});
    }
    printf("\"psi_series\":[");
    size_t step = psi_out.size() > 40 ? (psi_out.size() / 40 + 1) : 1;
    bool first_psi = true;
    for (size_t i = 0; i < psi_out.size(); i += step) {
        if (!first_psi) putchar(',');
        first_psi = false;
        printf("{\"ts_ms\":%lld,\"cpu\":%d,\"io\":%d,\"mem\":%d}",
               (long long)psi_out[i].ts_ms, psi_out[i].cpu, psi_out[i].io, psi_out[i].mem);
    }
    printf("],");

    printf("\"top_cpu\":[");
    for (size_t i = 0; i < b.sys.top_cpu.size() && i < 5; i++) {
        if (i > 0) putchar(',');
        printf("{\"comm\":\"%s\",\"pct\":%.1f}",
               json_escape(b.sys.top_cpu[i].comm).c_str(), b.sys.top_cpu[i].pct);
    }
    printf("],");

    std::vector<TailEntry> errs;
    if (b.rd.has_trace && !b.rd.trace_tails.empty()) {
        for (const auto& tt : b.rd.trace_tails) {
            if (tt.kind == TailKind::Error || tt.kind == TailKind::ModuleError) {
                errs.push_back({tt.ts_ms, 0, tt.source, "", tt.text, tt.kind});
            }
        }
    } else {
        for (const auto& te : b.tails) {
            if (te.kind == TailKind::Error || te.kind == TailKind::ModuleError) errs.push_back(te);
        }
    }
    printf("\"errors\":[");
    size_t err_start = errs.size() > 40 ? (errs.size() - 40) : 0;
    for (size_t i = err_start; i < errs.size(); i++) {
        if (i > err_start) putchar(',');
        const char* kstr = (errs[i].kind == TailKind::ModuleError) ? "MERR" : "ERR";
        printf("{\"ts_ms\":%lld,\"src\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\"}",
               (long long)errs[i].ts_ms, json_escape(errs[i].source).c_str(),
               kstr, json_escape(errs[i].text).c_str());
    }
    printf("]}\n");
    return 0;
}

int cmd_compare_json(const std::string& target_a, const std::string& target_b) {
    RuleSet rules;
    load_rules(&rules);
    RunSnapshotBundle a, b;
    if (!load_bundle(target_a, rules, &a) || !load_bundle(target_b, rules, &b)) {
        printf("{\"ok\":false,\"error\":\"run_not_found\"}\n");
        return 1;
    }

    auto print_run_head = [](const RunSnapshotBundle& x) {
        printf("{\"boot_id\":\"%s\",\"mtime_epoch\":%lld,\"bl_source\":\"%s\","
               "\"bl_total_ms\":%lld,\"first_frame_ms\":%lld,\"released_ms\":%lld,\"boot_done_ms\":%lld}",
               json_escape(x.boot_id).c_str(), (long long)x.mtime,
               json_escape(x.rd.bl_profile.source.empty() ? "NONE" : x.rd.bl_profile.source).c_str(),
               (long long)x.rd.bl_total_ms,
               (long long)map_get_i64(x.drend, "first_frame_submitted_ms", -1),
               (long long)map_get_i64(x.drend, "released_ms", -1),
               (long long)x.boot_done_ms);
    };

    printf("{\"ok\":true,\"run_a\":");
    print_run_head(a);
    printf(",\"run_b\":");
    print_run_head(b);

    std::vector<std::string> bl_ids;
    std::unordered_map<std::string, int64_t> bl_a, bl_b;
    for (const auto& s : a.rd.bl_profile.stages) {
        if (s.wait_only) continue;
        bl_ids.push_back(s.id);
        bl_a[s.id] = s.dur_ms;
    }
    for (const auto& s : b.rd.bl_profile.stages) {
        if (s.wait_only) continue;
        if (std::find(bl_ids.begin(), bl_ids.end(), s.id) == bl_ids.end()) bl_ids.push_back(s.id);
        bl_b[s.id] = s.dur_ms;
    }
    printf(",\"bl_deltas\":[");
    for (size_t i = 0; i < bl_ids.size(); i++) {
        if (i > 0) putchar(',');
        int64_t va = bl_a.count(bl_ids[i]) ? bl_a[bl_ids[i]] : -1;
        int64_t vb = bl_b.count(bl_ids[i]) ? bl_b[bl_ids[i]] : -1;
        int64_t d = (va >= 0 && vb >= 0) ? (vb - va) : 0;
        printf("{\"id\":\"%s\",\"a_ms\":%lld,\"b_ms\":%lld,\"delta_ms\":%lld,\"has_delta\":%s}",
               json_escape(bl_ids[i]).c_str(), (long long)va, (long long)vb,
               (long long)d, (va >= 0 && vb >= 0) ? "true" : "false");
    }
    printf("]");

    printf(",\"phase_deltas\":[");
    for (int i = 0; i < 6; i++) {
        if (i > 0) putchar(',');
        int64_t va = phase_dur(a.phases[i]);
        int64_t vb = phase_dur(b.phases[i]);
        int64_t d = (va >= 0 && vb >= 0) ? (vb - va) : 0;
        printf("{\"id\":\"%s\",\"a_ms\":%lld,\"b_ms\":%lld,\"delta_ms\":%lld,\"has_delta\":%s}",
               json_escape(a.phases[i].id).c_str(), (long long)va, (long long)vb,
               (long long)d, (va >= 0 && vb >= 0) ? "true" : "false");
    }
    printf("]");

    struct MsRow { std::string name; int64_t a; int64_t b; };
    std::vector<MsRow> ms_rows;
    auto push_ms_pair = [&](const std::string& nm, int64_t va, int64_t vb) {
        if (va < 0 && vb < 0) return;
        ms_rows.push_back({nm, va, vb});
    };
    push_ms_pair("display:started_ms",
                 map_get_i64(a.drend, "started_ms", -1), map_get_i64(b.drend, "started_ms", -1));
    push_ms_pair("display:first_frame_submitted_ms",
                 map_get_i64(a.drend, "first_frame_submitted_ms", -1),
                 map_get_i64(b.drend, "first_frame_submitted_ms", -1));
    push_ms_pair("display:released_ms",
                 map_get_i64(a.drend, "released_ms", -1), map_get_i64(b.drend, "released_ms", -1));

    std::vector<std::string> ev_tags;
    std::unordered_map<std::string, int64_t> ev_a, ev_b;
    for (const auto& e : a.rd.boot_events) { ev_tags.push_back(e.tag); ev_a[e.tag] = e.ts_ms; }
    for (const auto& e : b.rd.boot_events) {
        if (std::find(ev_tags.begin(), ev_tags.end(), e.tag) == ev_tags.end()) ev_tags.push_back(e.tag);
        ev_b[e.tag] = e.ts_ms;
    }
    for (const auto& t : ev_tags) {
        push_ms_pair("events:" + t, ev_a.count(t) ? ev_a[t] : -1, ev_b.count(t) ? ev_b[t] : -1);
    }

    printf(",\"milestone_deltas\":[");
    for (size_t i = 0; i < ms_rows.size(); i++) {
        if (i > 0) putchar(',');
        const auto& r = ms_rows[i];
        int64_t d = (r.a >= 0 && r.b >= 0) ? (r.b - r.a) : 0;
        printf("{\"name\":\"%s\",\"a_ms\":%lld,\"b_ms\":%lld,\"delta_ms\":%lld,\"has_delta\":%s}",
               json_escape(r.name).c_str(), (long long)r.a, (long long)r.b,
               (long long)d, (r.a >= 0 && r.b >= 0) ? "true" : "false");
    }
    printf("]");

    std::unordered_map<std::string, int64_t> svc_a, svc_b;
    for (const auto& s : extract_init_service_timings(a.rd)) {
        if (!svc_a.count(s.name) || s.dur_ms > svc_a[s.name]) svc_a[s.name] = s.dur_ms;
    }
    for (const auto& s : extract_init_service_timings(b.rd)) {
        if (!svc_b.count(s.name) || s.dur_ms > svc_b[s.name]) svc_b[s.name] = s.dur_ms;
    }
    struct SvcDelta { std::string name; int64_t a; int64_t b; int64_t delta; bool both; };
    std::vector<SvcDelta> sdeltas;
    for (const auto& kv : svc_a) {
        int64_t vb = svc_b.count(kv.first) ? svc_b[kv.first] : -1;
        sdeltas.push_back({kv.first, kv.second, vb, (vb >= 0 ? vb - kv.second : -kv.second), vb >= 0});
    }
    for (const auto& kv : svc_b) {
        if (!svc_a.count(kv.first)) {
            sdeltas.push_back({kv.first, -1, kv.second, kv.second, false});
        }
    }
    std::sort(sdeltas.begin(), sdeltas.end(), [](const SvcDelta& x, const SvcDelta& y) {
        return llabs(x.delta) > llabs(y.delta);
    });
    printf(",\"service_deltas\":[");
    for (size_t i = 0; i < sdeltas.size() && i < 15; i++) {
        if (i > 0) putchar(',');
        const auto& sd = sdeltas[i];
        printf("{\"name\":\"%s\",\"a_ms\":%lld,\"b_ms\":%lld,\"delta_ms\":%lld,\"has_delta\":%s}",
               json_escape(sd.name).c_str(), (long long)sd.a, (long long)sd.b,
               (long long)sd.delta, sd.both ? "true" : "false");
    }
    printf("]}\n");
    return 0;
}

int cmd_summary(const std::string& target) {
    RuleSet rules;
    load_rules(&rules);
    RunSnapshotBundle b;
    if (!load_bundle(target, rules, &b)) {
        printf("BootScope: no recorded boot found\n");
        return 1;
    }
    printf("=== BootScope Last Boot Replay ===\n");
    printf("dir=%s\n", b.dir.c_str());

    printf("\n[Metadata]\n");
    for (const auto& kv : b.meta) printf("%s=%s\n", kv.first.c_str(), kv.second.c_str());

    printf("\n[Display]\n");
    for (const auto& kv : b.dstat) printf("%s=%s\n", kv.first.c_str(), kv.second.c_str());
    for (const auto& kv : b.drend) printf("%s=%s\n", kv.first.c_str(), kv.second.c_str());

    printf("\n[Bootloader]\n");
    const auto& bp = b.rd.bl_profile;
    printf("  source=%s total_ms=%lld wait_ms=%lld %s\n",
           bp.source.empty() ? "NONE" : bp.source.c_str(),
           (long long)b.rd.bl_total_ms, (long long)bp.wait_ms, bp.meta_line.c_str());
    for (const auto& s : bp.stages) {
        printf("  %-8s start=%5lldms dur=%5lldms  %s\n",
               s.id.c_str(), (long long)s.start_ms, (long long)s.dur_ms, s.detail.c_str());
    }

    printf("\n[Critical Path]\n");
    for (int i = 0; i < 6; i++) {
        int64_t d = phase_dur(b.phases[i]);
        printf("  %-6s %-7s %6lldms  %s\n",
               b.phases[i].id, node_state_str(b.phases[i].state),
               (long long)d, b.phases[i].detail.c_str());
    }

    if (!b.rd.boot_events.empty()) {
        printf("\n[Framework Boot Events]\n");
        for (const auto& ev : b.rd.boot_events) {
            printf("  %8.3fs  %s\n", ev.ts_ms / 1000.0, ev.tag.c_str());
        }
    }

    auto svcs = extract_init_service_timings(b.rd);
    if (!svcs.empty()) {
        printf("\n[Top Init Services]\n");
        for (size_t i = 0; i < svcs.size() && i < 10; i++) {
            printf("  %7.3fs  %7.3fs  %-28s %s\n",
                   svcs[i].dur_ms / 1000.0, svcs[i].ts_ms / 1000.0,
                   svcs[i].name.c_str(), svcs[i].detail.c_str());
        }
    }
    printf("\n=== Replay Complete ===\n");
    return 0;
}

int cmd_prune_json(int keep) {
    int kept = 0, deleted = 0;
    if (prune_old_runs(keep, "", &kept, &deleted) != 0) {
        printf("{\"ok\":false,\"error\":\"invalid_keep\"}\n");
        return 1;
    }
    printf("{\"ok\":true,\"kept\":%d,\"deleted\":%d}\n", kept, deleted);
    return 0;
}

int cmd_config_get_json() {
    printf("{\"ok\":true,\"config\":");
    print_config_fields();
    printf("}\n");
    return 0;
}

int cmd_config_set_json(const std::string& key, const std::string& val) {
    if (key == "display_enabled") {
        bool en = (val == "1" || val == "true" || val == "on");
        if (!set_display_enabled(en)) {
            printf("{\"ok\":false,\"error\":\"write_failed\"}\n");
            return 1;
        }
    } else if (key == "max_history") {
        char* endp = nullptr;
        long n = strtol(val.c_str(), &endp, 10);
        if (val.empty() || endp == val.c_str() || *endp != '\0' || n < 0 || n > 200) {
            printf("{\"ok\":false,\"error\":\"invalid_max_history\"}\n");
            return 1;
        }
        if (!write_config_kv("max_history", std::to_string(n))) {
            printf("{\"ok\":false,\"error\":\"write_failed\"}\n");
            return 1;
        }
    } else {
        printf("{\"ok\":false,\"error\":\"unknown_key\"}\n");
        return 1;
    }
    return cmd_config_get_json();
}

int cmd_rules_get_json() {
    const char* src_label = "BUILTIN";
    const char* path = nullptr;
    if (access(kUserRulesPath, R_OK) == 0) {
        src_label = "USR";
        path = kUserRulesPath;
    } else if (access(kEarlyRulesPath, R_OK) == 0) {
        src_label = "DEF";
        path = kEarlyRulesPath;
    } else if (access(kModuleRulesPath, R_OK) == 0) {
        src_label = "DEF";
        path = kModuleRulesPath;
    }

    std::string content;
    if (path) {
        if (FILE* f = fopen(path, "r")) {
            char buf[4096];
            size_t n = 0;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && content.size() < 32768) {
                content.append(buf, n);
            }
            fclose(f);
        }
    }
    printf("{\"ok\":true,\"source\":\"%s\",\"content\":\"%s\"}\n",
           src_label, json_escape(content).c_str());
    return 0;
}

int cmd_rules_set_json() {
    std::string content;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
        if (content.size() + n > 32768) {
            printf("{\"ok\":false,\"error\":\"rules_too_large\"}\n");
            return 1;
        }
        content.append(buf, n);
    }

    std::string trimmed = trim_copy(content);
    if (trimmed.empty() || trimmed == "__RESET__") {
        unlink(kUserRulesPath);
        if (access(kModuleRulesPath, R_OK) == 0 && access("/metadata/vold/ksu", F_OK) == 0) {
            if (FILE* sf = fopen(kModuleRulesPath, "r")) {
                int fd = open(kEarlyRulesPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (fd >= 0) {
                    FILE* df = fdopen(fd, "w");
                    while (df && (n = fread(buf, 1, sizeof(buf), sf)) > 0) fwrite(buf, 1, n, df);
                    if (df) fclose(df);
                    else close(fd);
                }
                fclose(sf);
            }
        }
        return cmd_rules_get_json();
    }

    for (unsigned char c : content) {
        if (c == '\0' || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) {
            printf("{\"ok\":false,\"error\":\"invalid_control_char\"}\n");
            return 1;
        }
    }

    if (!mkdir_p(kBaseDir, 0700)) {
        printf("{\"ok\":false,\"error\":\"mkdir_failed\"}\n");
        return 1;
    }
    auto write_file_0644 = [&](const char* path) -> bool {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return false;
        size_t off = 0;
        while (off < content.size()) {
            ssize_t w = write(fd, content.data() + off, content.size() - off);
            if (w <= 0) { close(fd); return false; }
            off += (size_t)w;
        }
        fchmod(fd, 0644);
        close(fd);
        return true;
    };

    if (!write_file_0644(kUserRulesPath)) {
        printf("{\"ok\":false,\"error\":\"write_user_rules_failed\"}\n");
        return 1;
    }
    if (access("/metadata/vold/ksu", F_OK) == 0) {
        write_file_0644(kEarlyRulesPath);
    }
    return cmd_rules_get_json();
}

}  // namespace bootscope
