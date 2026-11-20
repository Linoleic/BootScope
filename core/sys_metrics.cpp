#include "sys_metrics.h"
#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <algorithm>

namespace bootscope {

bool read_psi_file(const char* path, PsiSample* out) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "some ", 5) == 0) {
            float avg10 = 0.0f;
            unsigned long long total = 0;
            const char* a = strstr(line, "avg10=");
            const char* t = strstr(line, "total=");
            if (a && t && sscanf(a, "avg10=%f", &avg10) == 1 && sscanf(t, "total=%llu", &total) == 1) {
                out->valid = true;
                out->avg10 = avg10;
                out->total_us = (uint64_t)total;
                ok = true;
            }
            break;
        }
    }
    fclose(f);
    return ok;
}

int compute_psi_pct(const PsiSample& cur, const PsiSample& prev, int64_t dt_ms) {
    if (!cur.valid) return -1;
    if (prev.valid && dt_ms > 0 && cur.total_us >= prev.total_us) {
        double pct = ((double)(cur.total_us - prev.total_us) / ((double)dt_ms * 1000.0)) * 100.0;
        if (pct < 0.0) pct = 0.0;
        if (pct > 100.0) pct = 100.0;
        return (int)lround(pct);
    }
    int avg = (int)lround(cur.avg10);
    if (avg < 0) avg = 0;
    if (avg > 100) avg = 100;
    return avg;
}

static int read_mem_total_gb() {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128];
    unsigned long long kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) break;
    }
    fclose(f);
    if (kb == 0) return 0;
    int gb = (int)lround((double)kb / 1048576.0);
    return gb > 0 ? gb : 1;
}

static std::string read_selinux_short() {
    FILE* f = fopen("/sys/fs/selinux/enforce", "r");
    if (!f) return "";
    char c = 0;
    size_t n = fread(&c, 1, 1, f);
    fclose(f);
    if (n != 1) return "";
    return (c == '1') ? "ENF" : "PERM";
}

static int read_active_module_count() {
    DIR* dir = opendir("/data/adb/modules");
    if (!dir) return -1;
    int cnt = 0;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string base = std::string("/data/adb/modules/") + ent->d_name;
        struct stat st {};
        if (stat(base.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        std::string dis = base + "/disable";
        std::string rem = base + "/remove";
        if (access(dis.c_str(), F_OK) == 0 || access(rem.c_str(), F_OK) == 0) continue;
        cnt++;
    }
    closedir(dir);
    return cnt;
}

DeviceInfo collect_device_info(const RuleSet& rules) {
    std::string model = get_prop("ro.product.model");
    if (model.empty()) model = get_prop("ro.product.vendor.model");
    std::string dev = get_prop("ro.product.device");
    if (dev.empty()) dev = get_prop("ro.hardware");
    std::string rel = get_prop("ro.build.version.release");
    std::string sdk = get_prop("ro.build.version.sdk");

    std::string raw1 = "DEV ";
    raw1 += model.empty() ? "ANDROID" : model;
    if (!dev.empty() && !iequals(model, dev)) {
        raw1 += " / " + dev;
    }
    if (!rel.empty() || !sdk.empty()) {
        raw1 += "  ";
        if (!rel.empty()) raw1 += "A" + rel;
        if (!rel.empty() && !sdk.empty()) raw1 += "/";
        if (!sdk.empty()) raw1 += "SDK" + sdk;
    }

    std::string abi = get_prop("ro.product.cpu.abi");
    std::string slot = get_prop("ro.boot.slot_suffix");
    if (!slot.empty() && slot[0] == '_') slot = slot.substr(1);
    std::string vbs = get_prop("ro.boot.verifiedbootstate");
    std::string sel = read_selinux_short();
    int mods = read_active_module_count();
    long cores = sysconf(_SC_NPROCESSORS_CONF);
    int ram_gb = read_mem_total_gb();

    struct utsname uts {};
    std::string krn;
    if (uname(&uts) == 0 && uts.release[0]) {
        krn = uts.release;
        size_t dash = krn.find('-');
        if (dash != std::string::npos) krn.resize(dash);
    }

    std::string raw2 = (abi.empty() || abi == "arm64-v8a") ? "ARM64" : abi;
    if (cores > 0) raw2 += " " + std::to_string(cores) + "C";
    if (ram_gb > 0) raw2 += " " + std::to_string(ram_gb) + "G";
    if (!krn.empty()) raw2 += " KRN " + krn;
    if (!slot.empty()) raw2 += " SL:" + slot;
    if (!vbs.empty()) raw2 += " VB:" + vbs;
    if (!sel.empty()) raw2 += " SE:" + sel;
    if (mods >= 0) raw2 += " M:" + std::to_string(mods);

    DeviceInfo info;
    info.line1 = sanitize_tail_text(rules, raw1, kTailLineChars);
    info.line2 = sanitize_tail_text(rules, raw2, kTailLineChars);
    if (info.line1.empty()) info.line1 = "DEV UNKNOWN";
    if (info.line2.empty()) info.line2 = "HW INFO N/A";
    return info;
}

static bool is_whole_block_dev(const char* name) {
    if (!name || !name[0]) return false;
    if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0
            || strncmp(name, "zram", 4) == 0 || strncmp(name, "dm-", 3) == 0) {
        return false;
    }
    size_t len = strlen(name);
    if (strncmp(name, "sd", 2) == 0 && len >= 3 && !isdigit((unsigned char)name[len - 1])) {
        return true;
    }
    if (strncmp(name, "mmcblk", 6) == 0) {
        for (size_t i = 6; i < len; i++) {
            if (!isdigit((unsigned char)name[i])) return false;
        }
        return len > 6;
    }
    if (strncmp(name, "nvme", 4) == 0) {
        const char* p = strchr(name, 'p');
        if (!p) return strchr(name, 'n') != nullptr;
        return false;
    }
    return false;
}

std::string read_cpu_freq_line() {
    std::vector<std::pair<int, unsigned long long>> freqs;
    DIR* dir = opendir("/sys/devices/system/cpu/cpufreq");
    if (dir) {
        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            int pol = -1;
            if (sscanf(ent->d_name, "policy%d", &pol) == 1) {
                std::string p = std::string("/sys/devices/system/cpu/cpufreq/") + ent->d_name + "/scaling_cur_freq";
                FILE* f = fopen(p.c_str(), "r");
                if (f) {
                    unsigned long long khz = 0;
                    if (fscanf(f, "%llu", &khz) == 1 && khz > 0) {
                        freqs.emplace_back(pol, khz);
                    }
                    fclose(f);
                }
            }
        }
        closedir(dir);
    }
    if (freqs.empty()) return "FREQ N/A";
    std::sort(freqs.begin(), freqs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out = "FREQ ";
    for (size_t i = 0; i < freqs.size() && i < 4; i++) {
        if (i > 0) out += "/";
        char b[16];
        snprintf(b, sizeof(b), "%.1f", freqs[i].second / 1000000.0);
        out += b;
    }
    out += "G";
    return out;
}

void read_mem_zram(std::string* out_line, bool* out_low) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) {
        *out_line = "MEM N/A";
        *out_low = false;
        return;
    }
    unsigned long long mem_total = 0, mem_avail = 0, swap_total = 0, swap_free = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v = 0;
        if (sscanf(line, "MemTotal: %llu kB", &v) == 1) mem_total = v;
        else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) mem_avail = v;
        else if (sscanf(line, "SwapTotal: %llu kB", &v) == 1) swap_total = v;
        else if (sscanf(line, "SwapFree: %llu kB", &v) == 1) swap_free = v;
    }
    fclose(f);
    if (mem_total == 0) {
        *out_line = "MEM N/A";
        *out_low = false;
        return;
    }
    double used_g = (mem_total > mem_avail ? (mem_total - mem_avail) : 0) / 1048576.0;
    double tot_g = mem_total / 1048576.0;
    double z_used_g = (swap_total > swap_free ? (swap_total - swap_free) : 0) / 1048576.0;
    double z_tot_g = swap_total / 1048576.0;
    char b[64];
    if (swap_total > 0) {
        snprintf(b, sizeof(b), "MEM %.1f/%.1fG  ZRAM %.2f/%.1fG", used_g, tot_g, z_used_g, z_tot_g);
    } else {
        snprintf(b, sizeof(b), "MEM %.1f/%.1fG  AVAIL %.1fG", used_g, tot_g, mem_avail / 1048576.0);
    }
    *out_line = b;
    *out_low = (mem_avail * 100ULL < mem_total * 15ULL);
}

void read_disk_io(int64_t now_ms, SysSampleState* st, std::string* out_line, bool* out_busy) {
    FILE* f = fopen("/proc/diskstats", "r");
    if (!f) {
        *out_line = "DISK N/A";
        *out_busy = false;
        return;
    }
    unsigned long long rsec = 0, wsec = 0, inflight = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned int maj = 0, min = 0;
        char name[64]{};
        unsigned long long rio = 0, rmerge = 0, r_s = 0, r_ms = 0;
        unsigned long long wio = 0, wmerge = 0, w_s = 0, w_ms = 0, q = 0;
        if (sscanf(line, " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &maj, &min, name, &rio, &rmerge, &r_s, &r_ms,
                   &wio, &wmerge, &w_s, &w_ms, &q) >= 12) {
            if (is_whole_block_dev(name)) {
                rsec += r_s;
                wsec += w_s;
                inflight += q;
            }
        }
    }
    fclose(f);
    double r_mbs = 0.0, w_mbs = 0.0;
    if (st->prev_io_ms > 0 && now_ms > st->prev_io_ms) {
        double dt_s = (now_ms - st->prev_io_ms) / 1000.0;
        if (rsec >= st->prev_read_sec) r_mbs = ((rsec - st->prev_read_sec) * 512.0 / 1048576.0) / dt_s;
        if (wsec >= st->prev_write_sec) w_mbs = ((wsec - st->prev_write_sec) * 512.0 / 1048576.0) / dt_s;
    }
    st->prev_read_sec = rsec;
    st->prev_write_sec = wsec;
    st->prev_io_ms = now_ms;
    double tot_mb = rsec * 512.0 / 1048576.0;
    char b[64];
    snprintf(b, sizeof(b), "DISK R:%.1f W:%.1fMB/S  TOT:%.0fM  Q:%llu",
             r_mbs, w_mbs, tot_mb, (unsigned long long)inflight);
    *out_line = b;
    *out_busy = (inflight >= 8 || r_mbs >= 150.0);
}

static uint64_t read_total_cpu_ticks() {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return 0;
    char line[256];
    unsigned long long sum = 0;
    if (fgets(line, sizeof(line), f)) {
        unsigned long long v[8]{};
        int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        for (int i = 0; i < n; i++) sum += v[i];
    }
    fclose(f);
    return (uint64_t)sum;
}

void read_top_cpu(SysSampleState* st, std::vector<TopProc>* out) {
    out->clear();
    uint64_t total_ticks = read_total_cpu_ticks();
    uint64_t dt_total = (st->prev_total_ticks > 0 && total_ticks > st->prev_total_ticks)
        ? (total_ticks - st->prev_total_ticks) : 0;

    std::unordered_map<int, uint64_t> cur_ticks;
    DIR* dir = opendir("/proc");
    if (!dir) return;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        int pid = atoi(ent->d_name);
        if (pid <= 0) continue;
        char path[48];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char buf[384];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        char* lp = strchr(buf, '(');
        char* rp = strrchr(buf, ')');
        if (!lp || !rp || rp <= lp + 1) continue;
        std::string comm(lp + 1, (size_t)(rp - lp - 1));
        const char* p = rp + 2;
        if (*p == '\0') continue;
        int field = 3;
        while (*p && field < 14) {
            if (*p == ' ') field++;
            p++;
        }
        unsigned long long ut = 0, st_t = 0;
        if (field == 14 && sscanf(p, "%llu %llu", &ut, &st_t) == 2) {
            uint64_t pt = (uint64_t)(ut + st_t);
            cur_ticks[pid] = pt;
            if (dt_total > 0) {
                auto it = st->prev_proc_ticks.find(pid);
                if (it != st->prev_proc_ticks.end() && pt > it->second) {
                    double pct = 100.0 * (double)(pt - it->second) / (double)dt_total;
                    if (pct >= 0.3) {
                        out->push_back({pid, upper_copy(comm), pct});
                    }
                }
            }
        }
    }
    closedir(dir);
    st->prev_total_ticks = total_ticks;
    st->prev_proc_ticks.swap(cur_ticks);
    std::sort(out->begin(), out->end(), [](const TopProc& a, const TopProc& b) {
        return a.pct > b.pct;
    });
    if (out->size() > 3) out->resize(3);
    for (auto& tp : *out) {
        char cpath[48];
        snprintf(cpath, sizeof(cpath), "/proc/%d/cmdline", tp.pid);
        int cfd = open(cpath, O_RDONLY | O_CLOEXEC);
        if (cfd >= 0) {
            char cbuf[256];
            ssize_t cn = read(cfd, cbuf, sizeof(cbuf) - 1);
            close(cfd);
            if (cn > 0) {
                cbuf[cn] = '\0';
                std::string arg0(cbuf);
                size_t sp = arg0.find(' ');
                if (sp != std::string::npos) arg0.resize(sp);
                size_t sl = arg0.rfind('/');
                if (sl != std::string::npos) arg0 = arg0.substr(sl + 1);
                if (!arg0.empty() && arg0 != "zygote" && arg0 != "zygote64") {
                    tp.comm = upper_copy(arg0);
                }
            }
        }
    }
}

SysMetrics collect_sys_metrics(int64_t now_ms, SysSampleState* st) {
    SysMetrics m;
    m.freq_line = read_cpu_freq_line();
    read_mem_zram(&m.mem_line, &m.mem_low);
    read_disk_io(now_ms, st, &m.io_line, &m.io_busy);
    read_top_cpu(st, &m.top_cpu);
    return m;
}

void populate_resource_rows(SysMetrics* m, int cpu_pct, int io_pct, int mem_pct) {
    m->rows.clear();
    std::string psi_str = "PSI N/A";
    if (cpu_pct >= 0 && io_pct >= 0 && mem_pct >= 0) {
        char pb[32];
        snprintf(pb, sizeof(pb), "PSI C:%2d%% I:%2d%% M:%2d%%", cpu_pct, io_pct, mem_pct);
        psi_str = pb;
    }
    m->rows.push_back({"PSI_FREQ", psi_str + "  " + m->freq_line, (io_pct >= 30 || cpu_pct >= 65)});
    m->rows.push_back({"MEM", m->mem_line, m->mem_low});
    m->rows.push_back({"DISK", m->io_line, m->io_busy});
}

}  // namespace bootscope
