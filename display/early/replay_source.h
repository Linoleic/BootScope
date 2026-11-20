#pragma once

#include "early/boot_sources.h"
#include "early/rules.h"
#include "early/sys_metrics.h"

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace bootscope {

struct ReplayInitEv {
    int64_t ts_ms = 0;
    std::string kind;
    std::string name;
    std::string detail;
};

struct ReplayBootEv {
    int64_t ts_ms = 0;
    std::string tag;
};

struct ReplayPsiPoint {
    int64_t ts_ms = 0;
    uint64_t cpu_t = 0;
    uint64_t io_t = 0;
    uint64_t mem_t = 0;
    float cpu_a = 0.0f;
    float io_a = 0.0f;
    float mem_a = 0.0f;
};

struct ReplayProcEntry {
    int pid = 0;
    std::string comm;
    uint64_t ticks = 0;
};

struct ReplayProcPoint {
    int64_t ts_ms = 0;
    uint64_t tot_ticks = 0;
    int mem_avail_mb = 0;
    int mem_tot_mb = 0;
    std::vector<ReplayProcEntry> procs;
};

struct ReplaySvcPoint {
    int64_t ts_ms = 0;
    std::string states[10];
};

struct ReplayTraceTail {
    int64_t ts_ms = 0;
    std::string source;
    TailKind kind = TailKind::Info;
    std::string text;
};

struct ReplayTraceMetric {
    int64_t ts_ms = 0;
    int cpu_pct = -1;
    int io_pct = -1;
    int mem_pct = -1;
    std::string freq_line;
    std::string mem_line;
    std::string io_line;
    std::vector<TopProc> top_cpu;
    std::string svc_states[10];
};

struct ReplayTracePhase {
    int64_t ts_ms = 0;
    int idx = 0;
    int state = 0;
    int64_t start_ms = -1;
    int64_t end_ms = -1;
    std::string detail;
};

struct ReplayTraceKv {
    int64_t ts_ms = 0;
    std::string key;
    std::string text;
    bool alert = false;
};

struct ReplayData {
    bool loaded = false;
    bool has_trace = false;
    int64_t pre_start_ms = 0;
    int64_t start_ms = 0;
    int64_t end_ms = 20000;
    std::string bl_line = "BOOTLOADER TIMING N/A";
    int64_t bl_total_ms = -1;
    std::vector<ReplayTracePhase> trace_phases;
    std::vector<ReplayTraceKv> trace_kvs;
    std::vector<ReplayTraceTail> trace_tails;
    std::vector<ReplayTraceMetric> trace_metrics;
    std::vector<ReplayInitEv> init_events;
    std::vector<ReplayBootEv> boot_events;
    std::vector<ReplayPsiPoint> psi_points;
    std::vector<ReplayProcPoint> proc_points;
    std::vector<ReplaySvcPoint> svc_points;
};

bool load_replay_data(const std::string& dir, ReplayData* out);

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
    std::string* last_transition_out);

}  // namespace bootscope
