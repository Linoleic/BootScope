#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>

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
constexpr const char* kDisableMarker = "/metadata/vold/ksu/bootscope_disable_display";
constexpr const char* kEarlyRulesPath = "/metadata/vold/ksu/bootscope_rules.conf";
constexpr const char* kUserRulesPath = "/data/adb/bootscope/rules.conf";
constexpr const char* kModuleRulesPath = "/data/adb/modules/bootscope/rules.conf";
constexpr const char* kTmpExecPath = "/tmp/bootscope_early";
constexpr const char* kPidPath = "/dev/bootscope_early.pid";
constexpr const char* kLogPath = "/dev/bootscope_early.log";
constexpr const char* kEarlyTracePath = "/dev/bootscope_early_trace.tsv";

int64_t boottime_ms();
std::string get_prop(const char* name);
std::string svc_state(const char* svc);
bool animation_active();

bool icontains(const std::string& hay, const char* needle);
bool iequals(const std::string& a, const std::string& b);
std::string trim_copy(const std::string& s);
std::string lower_copy(const std::string& s);
std::string upper_copy(const std::string& s);

}  // namespace bootscope
