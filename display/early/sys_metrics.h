#pragma once

#include "early/rules.h"

#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace bootscope {

struct PsiSample {
    bool valid = false;
    uint64_t total_us = 0;
    float avg10 = 0.0f;
};

struct DeviceInfo {
    std::string line1;
    std::string line2;
};

struct TopProc {
    int pid = 0;
    std::string comm;
    double pct = 0.0;
};

struct MetricRow {
    std::string key;
    std::string text;
    bool alert = false;
};

struct SysMetrics {
    std::string freq_line = "FREQ N/A";
    std::string mem_line = "MEM N/A";
    std::string io_line = "DISK N/A";
    bool io_busy = false;
    bool mem_low = false;
    std::vector<TopProc> top_cpu;
    std::vector<MetricRow> rows;
};

struct SysSampleState {
    uint64_t prev_read_sec = 0;
    uint64_t prev_write_sec = 0;
    int64_t prev_io_ms = 0;
    uint64_t prev_total_ticks = 0;
    std::unordered_map<int, uint64_t> prev_proc_ticks;
};

bool read_psi_file(const char* path, PsiSample* out);
int compute_psi_pct(const PsiSample& cur, const PsiSample& prev, int64_t dt_ms);
DeviceInfo collect_device_info(const RuleSet& rules);
SysMetrics collect_sys_metrics(int64_t now_ms, SysSampleState* st);
void populate_resource_rows(SysMetrics* m, int cpu_pct, int io_pct, int mem_pct);

}  // namespace bootscope
