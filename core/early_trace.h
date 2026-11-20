#pragma once

#include "bootloader.h"
#include "sys_metrics.h"

#include <stdint.h>
#include <string>

namespace bootscope {

void trace_open();
void trace_close();
bool trace_is_open();

void trace_life(int64_t ts_ms, const char* event, const std::string& detail = "");
void trace_bl(int64_t ts_ms, const std::string& bl_line, int64_t total_ms);
void trace_bl_meta(
    const std::string& source, int64_t timeline_ms, int64_t wait_ms, const std::string& meta_line);
void trace_bl_stage(const BlStage& st);
void trace_bl_event(const BlEvent& ev);
void trace_bl_profile(int64_t ts_ms, const BootloaderProfile& profile);
void trace_phase(
    int64_t ts_ms, int idx, int state, int64_t start_ms, int64_t end_ms,
    const std::string& detail);
void trace_stage(int64_t ts_ms, int step, const std::string& label, const std::string& raw_ms);
void trace_tail(int64_t ts_ms, const char* source, const char* kind, const std::string& text);
void trace_kv(int64_t ts_ms, const std::string& key, const std::string& text, bool alert);
void trace_metric(
    int64_t ts_ms, int cpu_pct, int io_pct, int mem_pct,
    const SysMetrics& sys, const std::string* cur_states, int svc_count);

}  // namespace bootscope
