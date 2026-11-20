#include "early/replay_source.h"
#include "early/common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <unordered_map>

namespace bootscope {

static std::string join_path(const std::string& dir, const char* file) {
    if (!dir.empty() && dir.back() == '/') return dir + file;
    return dir + "/" + file;
}

static void parse_bootloader_kpi(const std::string& path, ReplayData* out) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    uint64_t bl_start = 0, bl_end = 0, bl_load = 0, mpm_freq = 0;
    bool has_s = false, has_e = false, has_l = false, has_f = false;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char k[48]{};
        unsigned long long v = 0;
        if (sscanf(line, "%47s %llu", k, &v) == 2) {
            if (strcmp(k, "bl_start") == 0) { bl_start = v; has_s = true; }
            else if (strcmp(k, "bl_end") == 0) { bl_end = v; has_e = true; }
            else if (strcmp(k, "bl_load_kernel") == 0) { bl_load = v; has_l = true; }
            else if (strcmp(k, "mpm_freq") == 0) { mpm_freq = v; has_f = true; }
        }
    }
    fclose(f);
    if (has_s && has_e && bl_end >= bl_start && has_f && mpm_freq > 0) {
        int64_t tot_ms = (int64_t)llround(((double)(bl_end - bl_start) * 1000.0) / (double)mpm_freq);
        out->bl_total_ms = tot_ms;
        char buf[64];
        if (has_l && bl_load >= bl_start && bl_load <= bl_end) {
            int64_t pre = (int64_t)llround(((double)(bl_load - bl_start) * 1000.0) / (double)mpm_freq);
            int64_t kl = (int64_t)llround(((double)(bl_end - bl_load) * 1000.0) / (double)mpm_freq);
            snprintf(buf, sizeof(buf), "QCOM KPI PRE:%lldMS KL:%lldMS", (long long)pre, (long long)kl);
        } else {
            snprintf(buf, sizeof(buf), "QCOM KPI BL:%lldMS F:%llu", (long long)tot_ms, (unsigned long long)mpm_freq);
        }
        out->bl_line = buf;
    }
}

static std::string map_sample_svc_state(const std::string& v) {
    if (v == "running") return "RUN";
    if (v == "stopped") return "STOP";
    if (v == "restarting") return "RST";
    if (v == "stopping") return "STPG";
    return "-";
}

bool load_replay_data(const std::string& dir, ReplayData* out) {
    *out = ReplayData{};
    double max_sec = 0.0;
    int64_t started_ms = -1;
    int64_t first_frame_ms = -1;
    int64_t released_ms = -1;
    int64_t bootanim_done_ms = -1;

    if (FILE* f = fopen(join_path(dir, "display-renderer.txt").c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            long long v = 0;
            if (sscanf(line, "started_ms=%lld", &v) == 1 && v >= 0 && v <= 300000) {
                started_ms = (int64_t)v;
            } else if (sscanf(line, "first_frame_submitted_ms=%lld", &v) == 1 && v >= 0 && v <= 300000) {
                first_frame_ms = (int64_t)v;
            } else if (sscanf(line, "released_ms=%lld", &v) == 1 && v >= 0 && v <= 300000) {
                released_ms = (int64_t)v;
            }
        }
        fclose(f);
    }

    if (FILE* f = fopen(join_path(dir, "metadata.txt").c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            double fu = 0.0;
            if (sscanf(line, "finished_uptime=%lf", &fu) == 1 && fu > max_sec) max_sec = fu;
        }
        fclose(f);
    }

    parse_bootloader_kpi(join_path(dir, "bootloader-kpi.tsv"), out);

    if (FILE* f = fopen(join_path(dir, "init-events.tsv").c_str(), "r")) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty()) continue;
            size_t t1 = s.find('\t');
            size_t t2 = (t1 != std::string::npos) ? s.find('\t', t1 + 1) : std::string::npos;
            size_t t3 = (t2 != std::string::npos) ? s.find('\t', t2 + 1) : std::string::npos;
            if (t1 == std::string::npos || t2 == std::string::npos) continue;
            double sec = atof(s.substr(0, t1).c_str());
            if (sec > max_sec) max_sec = sec;
            ReplayInitEv ev;
            ev.ts_ms = (int64_t)llround(sec * 1000.0);
            ev.kind = s.substr(t1 + 1, t2 - t1 - 1);
            ev.name = (t3 == std::string::npos) ? s.substr(t2 + 1) : s.substr(t2 + 1, t3 - t2 - 1);
            ev.detail = (t3 == std::string::npos) ? "" : s.substr(t3 + 1);
            out->init_events.push_back(std::move(ev));
        }
        fclose(f);
    }

    if (FILE* f = fopen(join_path(dir, "boot-events.tsv").c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char tag[96]{};
            long long ms = 0;
            if (sscanf(line, "%95s %lld", tag, &ms) == 2 && ms >= 0) {
                if (ms / 1000.0 > max_sec) max_sec = ms / 1000.0;
                out->boot_events.push_back({(int64_t)ms, tag});
            }
        }
        fclose(f);
    }

    if (FILE* f = fopen(join_path(dir, "psi.tsv").c_str(), "r")) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            int idx = 0;
            double u = 0.0;
            unsigned long long ct = 0, cf = 0, it = 0, ifu = 0, mt = 0, mf = 0;
            float ca = 0.0f, ia = 0.0f, ma = 0.0f;
            if (sscanf(line, "%d %lf %llu %llu %f %llu %llu %f %llu %llu %f",
                       &idx, &u, &ct, &cf, &ca, &it, &ifu, &ia, &mt, &mf, &ma) == 11) {
                if (u > max_sec) max_sec = u;
                out->psi_points.push_back({(int64_t)llround(u * 1000.0), (uint64_t)ct, (uint64_t)it, (uint64_t)mt, ca, ia, ma});
            }
        }
        fclose(f);
    }

    if (FILE* f = fopen(join_path(dir, "proc.tsv").c_str(), "r")) {
        char line[512];
        ReplayProcPoint cur;
        bool has_cur = false;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') {
                if (has_cur) out->proc_points.push_back(std::move(cur));
                cur = ReplayProcPoint{};
                double u = 0.0;
                unsigned long long tot = 0;
                int ma = 0, mt = 0;
                const char* up = strstr(line, "uptime=");
                const char* tp = strstr(line, "tot=");
                const char* ap = strstr(line, "mem_avail_mb=");
                const char* mp = strstr(line, "mem_tot_mb=");
                if (up) sscanf(up, "uptime=%lf", &u);
                if (tp) sscanf(tp, "tot=%llu", &tot);
                if (ap) sscanf(ap, "mem_avail_mb=%d", &ma);
                if (mp) sscanf(mp, "mem_tot_mb=%d", &mt);
                cur.ts_ms = (int64_t)llround(u * 1000.0);
                cur.tot_ticks = (uint64_t)tot;
                cur.mem_avail_mb = ma;
                cur.mem_tot_mb = mt;
                has_cur = true;
            } else if (has_cur) {
                int pid = 0, ppid = 0;
                char comm[64]{}, st_c[8]{};
                unsigned long long ticks = 0;
                if (sscanf(line, "%d %d %63s %7s %llu", &pid, &ppid, comm, st_c, &ticks) == 5) {
                    cur.procs.push_back({pid, upper_copy(comm), (uint64_t)ticks});
                }
            }
        }
        if (has_cur) out->proc_points.push_back(std::move(cur));
        fclose(f);
    }

    if (FILE* f = fopen(join_path(dir, "samples.txt").c_str(), "r")) {
        char line[512];
        ReplaySvcPoint cur;
        bool has_cur = false;
        for (int i = 0; i < 10; i++) cur.states[i] = "-";
        while (fgets(line, sizeof(line), f)) {
            double u = 0.0;
            int sidx = 0;
            if (sscanf(line, "--- sample=%d uptime=%lf ---", &sidx, &u) == 2) {
                if (has_cur) out->svc_points.push_back(cur);
                cur = ReplaySvcPoint{};
                cur.ts_ms = (int64_t)llround(u * 1000.0);
                for (int i = 0; i < 10; i++) cur.states[i] = "-";
                if (u > max_sec) max_sec = u;
                has_cur = true;
            } else if (has_cur) {
                char key[96]{}, val[256]{};
                if (sscanf(line, "[%95[^]]]: [%255[^]]]", key, val) == 2) {
                    if (strcmp(key, "ro.boot.boottime") == 0 && out->bl_line == "BOOTLOADER TIMING N/A") {
                        out->bl_line = val;
                    }
                    static const char* const kKeys[9] = {
                        "init.svc.surfaceflinger", "init.svc.bootanim", "init.svc.zygote",
                        "init.svc.apexd", "init.svc.artd", "init.svc.wpa_supplicant",
                        "init.svc.vold", "init.svc.keystore2", "init.svc.installd"
                    };
                    for (int i = 0; i < 9; i++) {
                        if (strcmp(key, kKeys[i]) == 0) cur.states[i] = map_sample_svc_state(val);
                    }
                }
            }
        }
        if (has_cur) out->svc_points.push_back(cur);
        fclose(f);
    }

    if (FILE* f = fopen(join_path(dir, "early-trace.tsv").c_str(), "r")) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') continue;
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty()) continue;
            std::vector<std::string> cols;
            size_t pos = 0;
            while (true) {
                size_t tab = s.find('\t', pos);
                if (tab == std::string::npos) {
                    cols.push_back(s.substr(pos));
                    break;
                }
                cols.push_back(s.substr(pos, tab - pos));
                pos = tab + 1;
            }
            if (cols.size() < 3) continue;
            int64_t ts_ms = strtoll(cols[0].c_str(), nullptr, 10);
            if (ts_ms < 0 || ts_ms > 300000) continue;
            if (ts_ms / 1000.0 > max_sec) max_sec = ts_ms / 1000.0;
            const std::string& rtype = cols[1];
            if (rtype == "LIFE") {
                if (cols[2] == "started" && started_ms < 0) started_ms = ts_ms;
                else if (cols[2] == "first_frame" && first_frame_ms < 0) first_frame_ms = ts_ms;
                else if (cols[2] == "released") released_ms = ts_ms;
            } else if (rtype == "KV" && cols.size() >= 4) {
                ReplayTraceKv kv;
                kv.ts_ms = ts_ms;
                kv.key = cols[2];
                kv.text = cols[3];
                kv.alert = (cols.size() >= 5 && cols[4] == "1");
                out->trace_kvs.push_back(std::move(kv));
            } else if (rtype == "BL" && cols.size() >= 4) {
                int64_t bms = strtoll(cols[2].c_str(), nullptr, 10);
                if (bms >= 0) out->bl_total_ms = bms;
                if (!cols[3].empty()) out->bl_line = cols[3];
            } else if (rtype == "PHASE" && cols.size() >= 7) {
                ReplayTracePhase tp;
                tp.ts_ms = ts_ms;
                tp.idx = atoi(cols[2].c_str());
                tp.state = atoi(cols[3].c_str());
                tp.start_ms = strtoll(cols[4].c_str(), nullptr, 10);
                tp.end_ms = strtoll(cols[5].c_str(), nullptr, 10);
                tp.detail = cols[6];
                out->trace_phases.push_back(std::move(tp));
            } else if (rtype == "STAGE" && cols.size() >= 4) {
                ReplayTraceStage ts_row;
                ts_row.ts_ms = ts_ms;
                ts_row.step = atoi(cols[2].c_str());
                ts_row.label = cols[3];
                ts_row.raw_ms = (cols.size() >= 5) ? cols[4] : "";
                out->trace_stages.push_back(std::move(ts_row));
            } else if (rtype == "TAIL" && cols.size() >= 5) {
                ReplayTraceTail tt;
                tt.ts_ms = ts_ms;
                tt.source = cols[2];
                if (cols[3] == "STAGE") tt.kind = TailKind::Stage;
                else if (cols[3] == "ERR") tt.kind = TailKind::Error;
                else if (cols[3] == "MERR") tt.kind = TailKind::ModuleError;
                else tt.kind = TailKind::Info;
                tt.text = cols[4];
                out->trace_tails.push_back(std::move(tt));
            } else if (rtype == "METRIC" && cols.size() >= 8) {
                ReplayTraceMetric tm;
                tm.ts_ms = ts_ms;
                sscanf(cols[2].c_str(), "%d,%d,%d", &tm.cpu_pct, &tm.io_pct, &tm.mem_pct);
                tm.freq_line = cols[3];
                tm.mem_line = cols[4];
                tm.io_line = cols[5];
                if (cols[6] != "-") {
                    size_t p = 0;
                    while (p < cols[6].size()) {
                        size_t comma = cols[6].find(',', p);
                        std::string item = (comma == std::string::npos) ? cols[6].substr(p) : cols[6].substr(p, comma - p);
                        size_t colon = item.rfind(':');
                        if (colon != std::string::npos) {
                            tm.top_cpu.push_back({0, item.substr(0, colon), atof(item.substr(colon + 1).c_str())});
                        }
                        if (comma == std::string::npos) break;
                        p = comma + 1;
                    }
                }
                for (int i = 0; i < 10; i++) tm.svc_states[i] = "-";
                {
                    size_t p = 0;
                    int idx = 0;
                    while (p < cols[7].size() && idx < 10) {
                        size_t comma = cols[7].find(',', p);
                        tm.svc_states[idx++] = (comma == std::string::npos) ? cols[7].substr(p) : cols[7].substr(p, comma - p);
                        if (comma == std::string::npos) break;
                        p = comma + 1;
                    }
                }
                out->trace_metrics.push_back(std::move(tm));
            }
        }
        fclose(f);
        std::sort(out->trace_stages.begin(), out->trace_stages.end(),
                  [](const ReplayTraceStage& a, const ReplayTraceStage& b) { return a.ts_ms < b.ts_ms; });
        std::sort(out->trace_phases.begin(), out->trace_phases.end(),
                  [](const ReplayTracePhase& a, const ReplayTracePhase& b) { return a.ts_ms < b.ts_ms; });
        std::sort(out->trace_kvs.begin(), out->trace_kvs.end(),
                  [](const ReplayTraceKv& a, const ReplayTraceKv& b) { return a.ts_ms < b.ts_ms; });
        std::sort(out->trace_tails.begin(), out->trace_tails.end(),
                  [](const ReplayTraceTail& a, const ReplayTraceTail& b) { return a.ts_ms < b.ts_ms; });
        std::sort(out->trace_metrics.begin(), out->trace_metrics.end(),
                  [](const ReplayTraceMetric& a, const ReplayTraceMetric& b) { return a.ts_ms < b.ts_ms; });
        out->has_trace = !out->trace_tails.empty() || !out->trace_metrics.empty()
            || !out->trace_phases.empty() || !out->trace_kvs.empty() || !out->trace_stages.empty();
    }

    for (const auto& bev : out->boot_events) {
        if ((bev.tag == "wm_boot_animation_done" || bev.tag == "sf_stop_bootanim") && bev.ts_ms > bootanim_done_ms) {
            bootanim_done_ms = bev.ts_ms;
        }
    }

    out->start_ms = (first_frame_ms >= 0) ? first_frame_ms : 0;
    out->pre_start_ms = (started_ms >= 0 && started_ms < out->start_ms) ? started_ms : out->start_ms;
    if (released_ms > out->start_ms) {
        out->end_ms = released_ms;
    } else if (bootanim_done_ms > out->start_ms) {
        out->end_ms = bootanim_done_ms + 200;
    } else {
        out->end_ms = (int64_t)llround(max_sec * 1000.0);
    }
    if (out->end_ms <= out->start_ms) out->end_ms = out->start_ms + 5000;
    out->loaded = out->has_trace || !out->init_events.empty() || !out->boot_events.empty() || !out->svc_points.empty();
    return out->loaded;
}

static void apply_trace_kvs(const ReplayData& rd, int64_t sim_ms, SysMetrics* sys_out) {
    for (const auto& kv : rd.trace_kvs) {
        if (kv.ts_ms > sim_ms) break;
        bool updated = false;
        for (auto& r : sys_out->rows) {
            if (r.key == kv.key) {
                r.text = kv.text;
                r.alert = kv.alert;
                updated = true;
                break;
            }
        }
        if (!updated) {
            sys_out->rows.push_back({kv.key, kv.text, kv.alert});
        }
    }
}

static void sync_replay_stage(
        const ReplayData& rd, int64_t sim_ms, const PhaseNode* phases,
        const std::string* cur_states, StageInfo* stage_out) {
    for (const auto& ts_row : rd.trace_stages) {
        if (ts_row.ts_ms > sim_ms) break;
        stage_out->step = ts_row.step;
        stage_out->label = ts_row.label;
        stage_out->raw_ms = ts_row.raw_ms;
    }
    if (stage_out->step == 0) {
        if (phases[5].state == NodeState::Done) {
            *stage_out = {10, "ANIM_DONE", ""};
        } else if (phases[5].state == NodeState::Running) {
            *stage_out = {9, "AMS_READY", ""};
        } else if (phases[4].state == NodeState::Running) {
            *stage_out = {8, "PMS_START", ""};
        } else if (phases[3].state == NodeState::Running) {
            *stage_out = {5, "PRELOAD", ""};
        } else if (phases[2].state == NodeState::Running || cur_states[2] == "RUN") {
            *stage_out = {3, "ZYGOTE_START", ""};
        } else if (cur_states[3] == "RUN" || cur_states[3] == "STOP") {
            *stage_out = {2, "APEX_POST_FS", ""};
        } else {
            *stage_out = {1, "EARLY_BOOTANIM", ""};
        }
    }
}

void build_replay_snapshot(
        const ReplayData& rd, int64_t sim_ms, const RuleSet& rules,
        const char* const* short_names, int svc_count,
        PhaseNode* phases_out, StageInfo* stage_out,
        std::vector<std::pair<std::string, int64_t>>* init_ms_out,
        std::string* last_act_out, std::string* max_init_svc_out, int64_t* max_init_dur_out,
        std::string* wait_svc_out, int64_t* wait_dur_out, size_t* active_cnt_out,
        std::string* cur_states_out, int64_t* svc_start_ms_out,
        int* cpu_pct_out, int* io_pct_out, int* mem_pct_out,
        SysMetrics* sys_out, std::vector<TailEntry>* tail_out,
        std::string* last_transition_out) {
    const char* ids[] = {"BL", "INIT", "ZYG", "PRE", "PMS", "AMS"};
    for (int i = 0; i < 6; i++) {
        phases_out[i] = PhaseNode{};
        phases_out[i].id = ids[i];
    }
    phases_out[0].state = NodeState::Done;
    if (rd.bl_total_ms >= 0) {
        phases_out[0].start_ms = 0;
        phases_out[0].end_ms = rd.bl_total_ms;
    }
    phases_out[0].detail = rd.bl_line;

    phases_out[1].state = NodeState::Running;
    phases_out[1].start_ms = 0;
    phases_out[1].detail = "EARLY-INIT -> ZYGOTE";
    phases_out[2].detail = "ZYGOTE -> SYSTEM_SERVER";
    phases_out[3].detail = "PRELOAD CLASSES";
    phases_out[4].detail = "SYSTEM / DATA PKG SCAN";
    phases_out[5].detail = "AMS_READY -> SCREEN";

    *stage_out = StageInfo{};
    init_ms_out->clear();
    last_act_out->clear();
    max_init_svc_out->clear();
    *max_init_dur_out = 0;
    wait_svc_out->clear();
    *wait_dur_out = 0;
    *active_cnt_out = 0;
    tail_out->clear();

    std::vector<ActiveService> active;
    uint64_t seq = 1;
    auto push_replay_tail = [&](int64_t ts, const char* src, TailKind kind, const std::string& raw) {
        std::string txt = sanitize_tail_text(rules, raw, kTailLineChars - 8);
        if (txt.empty()) return;
        TailEntry e;
        e.ts_ms = ts;
        e.seq = seq++;
        e.source = src;
        e.text = txt;
        e.kind = kind;
        tail_out->push_back(std::move(e));
    };

    for (const auto& ev : rd.init_events) {
        if (ev.ts_ms > sim_ms) break;
        if (ev.kind == "phase") {
            *last_act_out = ev.name;
            bool seen = false;
            for (const auto& m : *init_ms_out) {
                if (m.first == ev.name) { seen = true; break; }
            }
            if (!seen && init_ms_out->size() < 8) init_ms_out->emplace_back(ev.name, ev.ts_ms);
            push_replay_tail(ev.ts_ms, "K", TailKind::Stage, "ACT " + ev.name);
        } else if (ev.kind == "start") {
            if (ev.name == "zygote") advance_phases_for_zygote(phases_out, ev.ts_ms);
            if (!is_resident_daemon(ev.name)) {
                active.push_back({ev.name, ev.ts_ms});
            }
            push_replay_tail(ev.ts_ms, "K", TailKind::Info, "+" + ev.name);
        } else if (ev.kind == "exit") {
            active.erase(std::remove_if(active.begin(), active.end(),
                                        [&](const ActiveService& a) { return a.name == ev.name; }),
                         active.end());
            const char* tp = strstr(ev.detail.c_str(), "took=");
            if (tp) {
                double took_s = atof(tp + 5);
                int64_t dur = (int64_t)llround(took_s * 1000.0);
                if (!is_resident_daemon(ev.name) && dur > *max_init_dur_out) {
                    *max_init_dur_out = dur;
                    *max_init_svc_out = ev.name;
                }
            }
            push_replay_tail(ev.ts_ms, "K", TailKind::Info, "-" + ev.name + " " + ev.detail);
        }
    }
    *active_cnt_out = active.size();
    for (const auto& a : active) {
        int64_t d = sim_ms - a.start_ms;
        if (d > *wait_dur_out) {
            *wait_dur_out = d;
            *wait_svc_out = a.name;
        }
    }

    for (const auto& bev : rd.boot_events) {
        if (bev.ts_ms > sim_ms) break;
        char msb[32];
        snprintf(msb, sizeof(msb), "%lld", (long long)bev.ts_ms);
        StageInfo next;
        if (map_framework_stage(bev.tag.c_str(), msb, &next)) {
            *stage_out = next;
            advance_phases_for_framework_tag(phases_out, bev.tag.c_str(), next, bev.ts_ms);
            push_replay_tail(bev.ts_ms, "P", TailKind::Stage, next.label);
        }
    }

    if (!rd.trace_phases.empty()) {
        for (const auto& tp : rd.trace_phases) {
            if (tp.ts_ms > sim_ms) break;
            if (tp.idx < 0 || tp.idx >= 6) continue;
            if (tp.state == 0) phases_out[tp.idx].state = NodeState::Pending;
            else if (tp.state == 1) phases_out[tp.idx].state = NodeState::Running;
            else phases_out[tp.idx].state = NodeState::Done;
            phases_out[tp.idx].start_ms = tp.start_ms;
            phases_out[tp.idx].end_ms = tp.end_ms;
            if (!tp.detail.empty()) phases_out[tp.idx].detail = tp.detail;
        }
    }

    if (rd.has_trace && !rd.trace_tails.empty()) {
        std::vector<TailEntry> merged;
        // Preserve synthesized framework P events in case trace hit cap or missed them.
        for (const auto& e : *tail_out) {
            if (e.source == "P") merged.push_back(e);
        }
        uint64_t tseq = 1000;
        for (const auto& tt : rd.trace_tails) {
            if (tt.ts_ms > sim_ms) break;
            bool dup = false;
            for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
                if (it->source == tt.source && it->text == tt.text) {
                    if (tt.kind == TailKind::Stage || (tt.ts_ms - it->ts_ms) < 2000) {
                        dup = true;
                    }
                    break;
                }
            }
            if (dup) continue;
            TailEntry e;
            e.ts_ms = tt.ts_ms;
            e.seq = tseq++;
            e.source = tt.source;
            e.text = tt.text;
            e.kind = tt.kind;
            merged.push_back(std::move(e));
        }
        *tail_out = std::move(merged);
    }

    if (rd.has_trace && !rd.trace_metrics.empty()) {
        for (int i = 0; i < svc_count; i++) {
            cur_states_out[i] = "-";
            svc_start_ms_out[i] = -1;
        }
        *last_transition_out = "REPLAY TRACE";
        const ReplayTraceMetric* last_tm = nullptr;
        for (const auto& tm : rd.trace_metrics) {
            if (tm.ts_ms > sim_ms) break;
            last_tm = &tm;
            for (int i = 0; i < svc_count && i < 10; i++) {
                if (cur_states_out[i] != tm.svc_states[i]) {
                    char tb[64];
                    snprintf(tb, sizeof(tb), "+%.2fS %s %s>%s",
                             tm.ts_ms / 1000.0, short_names[i], cur_states_out[i].c_str(), tm.svc_states[i].c_str());
                    *last_transition_out = tb;
                    cur_states_out[i] = tm.svc_states[i];
                    if (cur_states_out[i] == "RUN" && svc_start_ms_out[i] < 0) {
                        svc_start_ms_out[i] = tm.ts_ms;
                    }
                }
            }
        }
        if (last_tm) {
            *cpu_pct_out = last_tm->cpu_pct;
            *io_pct_out = last_tm->io_pct;
            *mem_pct_out = last_tm->mem_pct;
            *sys_out = SysMetrics{};
            sys_out->freq_line = last_tm->freq_line;
            sys_out->mem_line = last_tm->mem_line;
            sys_out->io_line = last_tm->io_line;
            sys_out->top_cpu = last_tm->top_cpu;
            populate_resource_rows(sys_out, *cpu_pct_out, *io_pct_out, *mem_pct_out);
        } else {
            populate_resource_rows(sys_out, -1, -1, -1);
        }
        apply_trace_kvs(rd, sim_ms, sys_out);
        sync_replay_stage(rd, sim_ms, phases_out, cur_states_out, stage_out);
        std::sort(tail_out->begin(), tail_out->end(), [](const TailEntry& a, const TailEntry& b) {
            if (a.ts_ms != b.ts_ms) return a.ts_ms < b.ts_ms;
            return a.seq < b.seq;
        });
        return;
    }

    for (int i = 0; i < svc_count; i++) {
        cur_states_out[i] = "-";
        svc_start_ms_out[i] = -1;
    }
    *last_transition_out = "REPLAY TIMELINE";
    for (const auto& sp : rd.svc_points) {
        if (sp.ts_ms > sim_ms) break;
        for (int i = 0; i < 9 && i < svc_count; i++) {
            if (cur_states_out[i] != sp.states[i]) {
                char tb[64];
                snprintf(tb, sizeof(tb), "+%.2fS %s %s>%s",
                         sp.ts_ms / 1000.0, short_names[i], cur_states_out[i].c_str(), sp.states[i].c_str());
                *last_transition_out = tb;
                cur_states_out[i] = sp.states[i];
                if (cur_states_out[i] == "RUN" && svc_start_ms_out[i] < 0) {
                    svc_start_ms_out[i] = sp.ts_ms;
                }
            }
        }
    }
    if (stage_out->step >= 4 && svc_count >= 10) {
        cur_states_out[9] = "RUN";
        if (svc_start_ms_out[9] < 0) svc_start_ms_out[9] = sim_ms;
    }

    *cpu_pct_out = -1;
    *io_pct_out = -1;
    *mem_pct_out = -1;
    for (size_t i = 0; i < rd.psi_points.size(); i++) {
        if (rd.psi_points[i].ts_ms > sim_ms) break;
        if (i == 0) {
            *cpu_pct_out = (int)lround(rd.psi_points[0].cpu_a);
            *io_pct_out = (int)lround(rd.psi_points[0].io_a);
            *mem_pct_out = (int)lround(rd.psi_points[0].mem_a);
        } else {
            int64_t dt = rd.psi_points[i].ts_ms - rd.psi_points[i - 1].ts_ms;
            if (dt > 0) {
                auto pct = [&](uint64_t cur_t, uint64_t prev_t) -> int {
                    if (cur_t < prev_t) return 0;
                    int v = (int)lround(((double)(cur_t - prev_t) / ((double)dt * 1000.0)) * 100.0);
                    return std::min(100, std::max(0, v));
                };
                *cpu_pct_out = pct(rd.psi_points[i].cpu_t, rd.psi_points[i - 1].cpu_t);
                *io_pct_out = pct(rd.psi_points[i].io_t, rd.psi_points[i - 1].io_t);
                *mem_pct_out = pct(rd.psi_points[i].mem_t, rd.psi_points[i - 1].mem_t);
            }
        }
    }

    *sys_out = SysMetrics{};
    sys_out->freq_line = "MODE REPLAY";
    sys_out->io_line = "DISK REPLAY";
    const ReplayProcPoint* cur_pp = nullptr;
    const ReplayProcPoint* prev_pp = nullptr;
    for (size_t i = 0; i < rd.proc_points.size(); i++) {
        if (rd.proc_points[i].ts_ms > sim_ms) break;
        prev_pp = cur_pp;
        cur_pp = &rd.proc_points[i];
    }
    if (cur_pp) {
        if (cur_pp->mem_tot_mb > 0) {
            double used_g = (cur_pp->mem_tot_mb > cur_pp->mem_avail_mb
                             ? (cur_pp->mem_tot_mb - cur_pp->mem_avail_mb) : 0) / 1024.0;
            double tot_g = cur_pp->mem_tot_mb / 1024.0;
            char mb[48];
            snprintf(mb, sizeof(mb), "MEM %.1f/%.1fG  AVAIL %dM", used_g, tot_g, cur_pp->mem_avail_mb);
            sys_out->mem_line = mb;
        }
        if (prev_pp && cur_pp->tot_ticks > prev_pp->tot_ticks) {
            uint64_t dt = cur_pp->tot_ticks - prev_pp->tot_ticks;
            std::unordered_map<int, uint64_t> prev_map;
            for (const auto& pe : prev_pp->procs) prev_map[pe.pid] = pe.ticks;
            for (const auto& pe : cur_pp->procs) {
                auto it = prev_map.find(pe.pid);
                uint64_t prev_t = (it != prev_map.end()) ? it->second : 0;
                if (pe.ticks > prev_t) {
                    double pct = 100.0 * (double)(pe.ticks - prev_t) / (double)dt;
                    if (pct >= 0.3) sys_out->top_cpu.push_back({pe.pid, pe.comm, pct});
                }
            }
            std::sort(sys_out->top_cpu.begin(), sys_out->top_cpu.end(),
                      [](const TopProc& a, const TopProc& b) { return a.pct > b.pct; });
            if (sys_out->top_cpu.size() > 3) sys_out->top_cpu.resize(3);
        }
    }
    populate_resource_rows(sys_out, *cpu_pct_out, *io_pct_out, *mem_pct_out);
    apply_trace_kvs(rd, sim_ms, sys_out);
    sync_replay_stage(rd, sim_ms, phases_out, cur_states_out, stage_out);

    std::sort(tail_out->begin(), tail_out->end(), [](const TailEntry& a, const TailEntry& b) {
        if (a.ts_ms != b.ts_ms) return a.ts_ms < b.ts_ms;
        return a.seq < b.seq;
    });
}

}  // namespace bootscope
