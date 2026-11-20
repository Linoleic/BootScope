#pragma once

#include "early/bootloader.h"
#include "early/rules.h"

#include <stdint.h>
#include <sys/types.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace bootscope {

struct StageInfo {
    int step = 0;
    std::string label = "EARLY_BOOT";
    std::string raw_ms;
};

enum class TailKind {
    Info,
    Stage,
    Error,
    ModuleError,
};

struct TailEntry {
    int64_t ts_ms = 0;
    uint64_t seq = 0;
    std::string source;
    std::string group;
    std::string text;
    TailKind kind = TailKind::Info;
};

enum class NodeState {
    Pending,
    Running,
    Done,
};

struct PhaseNode {
    const char* id = "";
    NodeState state = NodeState::Pending;
    int64_t start_ms = -1;
    int64_t end_ms = -1;
    std::string detail;
};

struct ActiveService {
    std::string name;
    int64_t start_ms = 0;
};

struct EventState {
    std::mutex mu;
    RuleSet rules;
    StageInfo framework_stage;
    PhaseNode phases[6];
    QcomKpi qcom_kpi;
    BootloaderProfile bl_profile;
    std::vector<ActiveService> active_svcs;
    std::string max_init_svc;
    int64_t max_init_dur_ms = 0;
    std::string last_init_action;
    std::vector<std::pair<std::string, int64_t>> init_milestones;
    std::vector<TailEntry> tail;
    uint64_t next_seq = 1;
    pid_t logcat_pid = -1;
    pid_t err_logcat_pid = -1;
    volatile bool stop = false;
};

bool map_framework_stage(const char* tag, const char* ms, StageInfo* out);
StageInfo infer_native_stage(const std::string& apex_state, const std::string& zyg_state);
void advance_phases_for_zygote(PhaseNode* phases, int64_t ts_ms);
void advance_phases_for_framework_tag(
    PhaseNode* phases, const char* tag, const StageInfo& next, int64_t ts_ms);

void push_tail(
    EventState* st, int64_t ts_ms, const char* source, TailKind kind,
    const std::string& raw_text, const std::string& group = "");

bool is_resident_daemon(const std::string& svc);
bool is_stack_trace_noise(const char* msg);
void* kmsg_reader_thread(void* arg);
void* err_reader_thread(void* arg);
void* event_reader_thread(void* arg);

std::string build_bootloader_line();
std::string short_init_action(const std::string& act);
void init_phases(EventState* st, const BootloaderProfile& bl_profile);
std::string format_phase_row(const PhaseNode& n, int64_t now_ms, const RuleSet& rules);
std::string format_tail_row(const TailEntry& e);

}  // namespace bootscope
