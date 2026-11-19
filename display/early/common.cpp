#include "early/common.h"

#include <ctype.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>

namespace bootscope {

int64_t boottime_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

std::string get_prop(const char* name) {
    const prop_info* pi = __system_property_find(name);
    if (!pi) return "";
    std::string result;
    __system_property_read_callback(
        pi,
        [](void* cookie, const char*, const char* value, uint32_t) {
            if (value) *(std::string*)cookie = value;
        },
        &result);
    return result;
}

std::string svc_state(const char* svc) {
    std::string key = std::string("init.svc.") + svc;
    std::string v = get_prop(key.c_str());
    if (v == "running") return "RUN";
    if (v == "stopped") return "STOP";
    if (v == "restarting") return "RST";
    if (v == "stopping") return "STPG";
    return "-";
}

bool animation_active() {
    return get_prop("init.svc.bootanim") == "running"
        && get_prop("service.bootanim.exit") != "1"
        && get_prop("sys.boot_completed") != "1";
}

bool icontains(const std::string& hay, const char* needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    if (hay.size() < nlen) return false;
    for (size_t i = 0; i + nlen <= hay.size(); i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

std::string trim_copy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        start++;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        end--;
    }
    return s.substr(start, end - start);
}

std::string lower_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back((char)tolower(c));
    return out;
}

std::string upper_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back((char)toupper(c));
    return out;
}

}  // namespace bootscope
