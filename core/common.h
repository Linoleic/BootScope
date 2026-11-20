#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <string>
#include <vector>

namespace bootscope {

constexpr int kPortraitWidth = 760;
constexpr int kPortraitHeight = 920;
constexpr int kLandWidth = 1280;
constexpr int kLandHeight = 680;
constexpr int32_t kLayer = 0x40000001;
constexpr int kStageTotal = 10;
constexpr size_t kMaxTailRows = 14;
constexpr size_t kTailLineChars = 59;

constexpr const char* kLayerName = "BootScopeDisplay";
constexpr const char* kBaseDir = "/data/adb/bootscope";
constexpr const char* kConfigPath = "/data/adb/bootscope/config.conf";
constexpr const char* kDisableMarkerUser = "/data/adb/bootscope/disable_display";
constexpr const char* kDisableMarkerEarly = "/metadata/vold/ksu/bootscope_disable_display";
constexpr const char* kDisableMarker = kDisableMarkerEarly;
constexpr const char* kEarlyBinPath = "/metadata/vold/ksu/bootscope";
constexpr const char* kEarlyRulesPath = "/metadata/vold/ksu/bootscope_rules.conf";
constexpr const char* kUserRulesPath = "/data/adb/bootscope/rules.conf";
constexpr const char* kModuleRulesPath = "/data/adb/modules/bootscope/rules.conf";
constexpr const char* kTmpExecPath = "/tmp/bootscope";
constexpr const char* kPidPath = "/dev/bootscope_early.pid";
constexpr const char* kLogPath = "/dev/bootscope_early.log";
constexpr const char* kEarlyTracePath = "/dev/bootscope_early_trace.tsv";

struct RunDirEntry {
    std::string boot_id;
    std::string path;
    time_t mtime = 0;
    long mtime_nsec = 0;
};

int64_t boottime_ms();
std::string get_prop(const char* name);
std::string svc_state(const char* svc);
bool animation_active();

bool icontains(const std::string& hay, const char* needle);
bool iequals(const std::string& a, const std::string& b);
std::string trim_copy(const std::string& s);
std::string lower_copy(const std::string& s);
std::string upper_copy(const std::string& s);

bool is_valid_uuid(const std::string& s);
std::string json_escape(const std::string& s);
bool mkdir_p(const std::string& path, mode_t mode);
bool has_run_artifacts(const std::string& dir);
std::vector<RunDirEntry> list_run_dirs_sorted();
std::string find_latest_run_dir();
std::string resolve_run_dir(const std::string& arg);

int read_config_int(const char* key, int default_val);
bool write_config_kv(const std::string& key, const std::string& val);
bool is_display_disabled();
bool set_display_enabled(bool enabled);

}  // namespace bootscope
