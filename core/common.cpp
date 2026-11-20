#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <utility>

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

bool is_valid_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    return out;
}

bool mkdir_p(const std::string& path, mode_t mode) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/") continue;
            if (mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

bool has_run_artifacts(const std::string& dir) {
    static const char* const kMarkers[] = {
        "/metadata.txt", "/samples.txt", "/early-trace.tsv", "/display-renderer.txt"
    };
    for (const char* m : kMarkers) {
        if (access((dir + m).c_str(), F_OK) == 0) return true;
    }
    return false;
}

std::vector<RunDirEntry> list_run_dirs_sorted() {
    std::vector<RunDirEntry> out;
    DIR* dir = opendir(kBaseDir);
    if (!dir) return out;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name(ent->d_name);
        if (!is_valid_uuid(name)) continue;
        std::string full = std::string(kBaseDir) + "/" + name;
        struct stat st {};
        if (stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (!has_run_artifacts(full)) continue;
        RunDirEntry e;
        e.boot_id = name;
        e.path = full;
        e.mtime = st.st_mtim.tv_sec;
        e.mtime_nsec = st.st_mtim.tv_nsec;
        out.push_back(std::move(e));
    }
    closedir(dir);
    std::sort(out.begin(), out.end(), [](const RunDirEntry& a, const RunDirEntry& b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        if (a.mtime_nsec != b.mtime_nsec) return a.mtime_nsec > b.mtime_nsec;
        return a.boot_id > b.boot_id;
    });
    return out;
}

std::string find_latest_run_dir() {
    auto runs = list_run_dirs_sorted();
    return runs.empty() ? std::string() : runs[0].path;
}

std::string resolve_run_dir(const std::string& arg) {
    if (arg.empty() || arg == "latest") {
        return find_latest_run_dir();
    }
    if (is_valid_uuid(arg)) {
        return std::string(kBaseDir) + "/" + arg;
    }
    return arg;
}

int read_config_int(const char* key, int default_val) {
    FILE* f = fopen(kConfigPath, "r");
    if (!f) return default_val;
    int result = default_val;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim_copy(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(s.substr(0, eq));
        std::string v = trim_copy(s.substr(eq + 1));
        if (k == key && !v.empty()) {
            char* endp = nullptr;
            long val = strtol(v.c_str(), &endp, 10);
            if (endp != v.c_str() && *endp == '\0') {
                result = (int)val;
            }
            break;
        }
    }
    fclose(f);
    return result;
}

bool write_config_kv(const std::string& key, const std::string& val) {
    if (!mkdir_p(kBaseDir, 0700)) return false;
    std::vector<std::pair<std::string, std::string>> kvs;
    if (FILE* rf = fopen(kConfigPath, "r")) {
        char line[256];
        while (fgets(line, sizeof(line), rf)) {
            std::string s = trim_copy(line);
            if (s.empty() || s[0] == '#') continue;
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kvs.emplace_back(trim_copy(s.substr(0, eq)), trim_copy(s.substr(eq + 1)));
        }
        fclose(rf);
    }
    bool replaced = false;
    for (auto& kv : kvs) {
        if (kv.first == key) {
            kv.second = val;
            replaced = true;
            break;
        }
    }
    if (!replaced) kvs.emplace_back(key, val);

    int fd = open(kConfigPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    FILE* wf = fdopen(fd, "w");
    if (!wf) {
        close(fd);
        return false;
    }
    for (const auto& kv : kvs) {
        fprintf(wf, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    }
    fclose(wf);
    return true;
}

bool is_display_disabled() {
    return access(kDisableMarkerUser, F_OK) == 0 || access(kDisableMarkerEarly, F_OK) == 0;
}

bool set_display_enabled(bool enabled) {
    if (enabled) {
        unlink(kDisableMarkerUser);
        unlink(kDisableMarkerEarly);
        return true;
    }
    if (!mkdir_p(kBaseDir, 0700)) return false;
    int fd1 = open(kDisableMarkerUser, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd1 < 0) return false;
    close(fd1);
    if (access("/metadata/vold/ksu", F_OK) == 0) {
        int fd2 = open(kDisableMarkerEarly, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd2 >= 0) close(fd2);
    }
    return true;
}

}  // namespace bootscope
