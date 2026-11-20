#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace bootscope {

struct QcomKpi {
    bool has_start = false;
    bool has_end = false;
    bool has_load = false;
    bool has_kernel = false;
    bool has_freq = false;
    uint64_t bl_start = 0;
    uint64_t bl_end = 0;
    uint64_t bl_load_kernel = 0;
    uint64_t kernel_mpm = 0;
    uint64_t mpm_freq = 0;
};

struct BlStage {
    std::string id;
    std::string detail;
    int64_t start_ms = 0;
    int64_t dur_ms = 0;
    bool wait_only = false;
};

struct BlEvent {
    int64_t por_ms = 0;
    bool major = false;
    std::string text;
};

struct BootloaderProfile {
    std::string source;
    std::string summary_line = "BOOTLOADER TIMING N/A";
    std::string meta_line;
    int64_t total_ms = -1;
    int64_t timeline_ms = -1;
    int64_t wait_ms = 0;
    std::vector<BlStage> stages;
    std::vector<BlEvent> events;
};

bool parse_boottime_props(
    const std::string& boottime, const std::string& boottime_extra, BootloaderProfile* out);
bool parse_qcom_uefilog_file(const std::string& path, BootloaderProfile* out);
bool build_qcom_kpi_profile(const QcomKpi& k, BootloaderProfile* out);
bool merge_qcom_kpi_into_uefilog(const QcomKpi& k, BootloaderProfile* profile);
bool parse_mtk_bootprof_file(const std::string& path, BootloaderProfile* out);
bool parse_qcom_sysfs_kpi_file(const std::string& path, BootloaderProfile* out);
std::string collect_bootconfig_meta();
BootloaderProfile collect_bootloader_profile();

}  // namespace bootscope
