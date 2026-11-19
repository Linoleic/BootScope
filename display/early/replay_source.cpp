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

    out->end_ms = (int64_t)llround(max_sec * 1000.0);
    if (out->end_ms < 5000) out->end_ms = 5000;
    out->loaded = !out->init_events.empty() || !out->boot_events.empty() || !out->svc_points.empty();
    return out->loaded;
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
            if (ev.name == "zygote" && phases_out[1].state == NodeState::Running) {
                phases_out[1].state = NodeState::Done;
                phases_out[1].end_ms = ev.ts_ms;
                phases_out[2].state = NodeState::Running;
                phases_out[2].start_ms = ev.ts_ms;
            }
            active.push_back({ev.name, ev.ts_ms});
            push_replay_tail(ev.ts_ms, "K", TailKind::Info, "+" + ev.name);
        } else if (ev.kind == "exit") {
            active.erase(std::remove_if(active.begin(), active.end(),
                                        [&](const ActiveService& a) { return a.name == ev.name; }),
                         active.end());
            const char* tp = strstr(ev.detail.c_str(), "took=");
            if (tp) {
                double took_s = atof(tp + 5);
                int64_t dur = (int64_t)llround(took_s * 1000.0);
                if (dur > *max_init_dur_out) {
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
            if (bev.tag == "boot_progress_start") {
                phases_out[1].state = NodeState::Done;
                if (phases_out[1].end_ms < 0) phases_out[1].end_ms = bev.ts_ms;
                if (phases_out[2].start_ms < 0) phases_out[2].start_ms = 0;
                phases_out[2].state = NodeState::Done;
                phases_out[2].end_ms = bev.ts_ms;
                phases_out[2].detail = "SYSTEM_SERVER STARTED";
                phases_out[3].state = NodeState::Running;
                phases_out[3].start_ms = bev.ts_ms;
            } else if (bev.tag == "boot_progress_preload_start") {
                phases_out[3].state = NodeState::Running;
                phases_out[3].start_ms = bev.ts_ms;
            } else if (bev.tag == "boot_progress_preload_end") {
                if (phases_out[3].start_ms < 0) phases_out[3].start_ms = bev.ts_ms;
                phases_out[3].state = NodeState::Done;
                phases_out[3].end_ms = bev.ts_ms;
                phases_out[4].state = NodeState::Running;
                phases_out[4].start_ms = bev.ts_ms;
            } else if (bev.tag == "boot_progress_pms_ready") {
                if (phases_out[4].start_ms < 0) phases_out[4].start_ms = bev.ts_ms;
                phases_out[4].state = NodeState::Done;
                phases_out[4].end_ms = bev.ts_ms;
                phases_out[5].state = NodeState::Running;
                phases_out[5].start_ms = bev.ts_ms;
            } else if (bev.tag == "boot_progress_enable_screen" || bev.tag == "wm_boot_animation_done") {
                if (phases_out[5].start_ms < 0) phases_out[5].start_ms = bev.ts_ms;
                phases_out[5].state = NodeState::Done;
                phases_out[5].end_ms = bev.ts_ms;
                phases_out[5].detail = next.label;
            }
            push_replay_tail(bev.ts_ms, "P", TailKind::Stage, next.label);
        }
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

    std::sort(tail_out->begin(), tail_out->end(), [](const TailEntry& a, const TailEntry& b) {
        if (a.ts_ms != b.ts_ms) return a.ts_ms < b.ts_ms;
        return a.seq < b.seq;
    });
}

}  // namespace bootscope
