#include "collector.h"
#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bootscope {
namespace {

double read_uptime_sec() {
    FILE* f = fopen("/proc/uptime", "r");
    if (!f) return 0.0;
    double up = 0.0;
    if (fscanf(f, "%lf", &up) != 1) up = 0.0;
    fclose(f);
    return up;
}

bool remove_dir_recursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        std::string child = path + "/" + ent->d_name;
        struct stat st {};
        if (lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            remove_dir_recursive(child);
        } else {
            unlink(child.c_str());
        }
    }
    closedir(dir);
    return rmdir(path.c_str()) == 0;
}

bool is_collected_prop(const char* name) {
    if (strncmp(name, "init.svc.", 9) == 0) return true;
    if (strncmp(name, "ro.boottime.", 12) == 0) return true;
    return strcmp(name, "ro.boot.boottime") == 0
        || strcmp(name, "ro.boot.boottime_extra") == 0
        || strcmp(name, "sys.boot_completed") == 0
        || strcmp(name, "dev.bootcomplete") == 0
        || strcmp(name, "service.bootanim.exit") == 0;
}

void write_props_sample(FILE* out, int sample_idx, double uptime_s) {
    std::vector<std::pair<std::string, std::string>> props;
    __system_property_foreach(
        [](const prop_info* pi, void* cookie) {
            auto* vec = (std::vector<std::pair<std::string, std::string>>*)cookie;
            __system_property_read_callback(
                pi,
                [](void* c2, const char* name, const char* value, uint32_t) {
                    if (!name || !value || !is_collected_prop(name)) return;
                    auto* v = (std::vector<std::pair<std::string, std::string>>*)c2;
                    v->emplace_back(name, value);
                },
                vec);
        },
        &props);
    std::sort(props.begin(), props.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    fprintf(out, "--- sample=%d uptime=%.2f ---\n", sample_idx, uptime_s);
    for (const auto& kv : props) {
        fprintf(out, "[%s]: [%s]\n", kv.first.c_str(), kv.second.c_str());
    }
    fflush(out);
}

struct PsiCounter {
    uint64_t some_total = 0;
    uint64_t full_total = 0;
    double some_avg10 = 0.0;
};

bool parse_psi_counters(const char* path, PsiCounter* out) {
    *out = PsiCounter{};
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char* t = strstr(line, "total=");
        const char* a = strstr(line, "avg10=");
        unsigned long long tot = 0;
        double avg = 0.0;
        if (strncmp(line, "some ", 5) == 0) {
            if (t && sscanf(t, "total=%llu", &tot) == 1) out->some_total = tot;
            if (a && sscanf(a, "avg10=%lf", &avg) == 1) out->some_avg10 = avg;
        } else if (strncmp(line, "full ", 5) == 0) {
            if (t && sscanf(t, "total=%llu", &tot) == 1) out->full_total = tot;
        }
    }
    fclose(f);
    return true;
}

void write_psi_row(FILE* out, int sample_idx, double uptime_s, int* psi_rows) {
    PsiCounter c, i, m;
    bool ok = parse_psi_counters("/proc/pressure/cpu", &c);
    ok = parse_psi_counters("/proc/pressure/io", &i) && ok;
    ok = parse_psi_counters("/proc/pressure/memory", &m) && ok;
    if (!ok) return;
    fprintf(out, "%d\t%.2f\t%llu\t%llu\t%.2f\t%llu\t%llu\t%.2f\t%llu\t%llu\t%.2f\n",
            sample_idx, uptime_s,
            (unsigned long long)c.some_total, (unsigned long long)c.full_total, c.some_avg10,
            (unsigned long long)i.some_total, (unsigned long long)i.full_total, i.some_avg10,
            (unsigned long long)m.some_total, (unsigned long long)m.full_total, m.some_avg10);
    fflush(out);
    (*psi_rows)++;
}

void write_proc_snapshot(FILE* out, int sample_idx, double uptime_s, int* proc_rows) {
    uint64_t tot = 0, idle = 0, iow = 0;
    if (FILE* f = fopen("/proc/stat", "r")) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            unsigned long long v[8]{};
            int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
            for (int k = 0; k < n; k++) tot += v[k];
            if (n >= 5) {
                idle = v[3] + v[4];
                iow = v[4];
            }
        }
        fclose(f);
    }

    int mem_tot_mb = 0, mem_avail_mb = 0;
    if (FILE* f = fopen("/proc/meminfo", "r")) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            unsigned long long kb = 0;
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) mem_tot_mb = (int)(kb / 1024ULL);
            else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) mem_avail_mb = (int)(kb / 1024ULL);
        }
        fclose(f);
    }

    fprintf(out, "# sample=%d uptime=%.2f tot=%llu idle=%llu iow=%llu mem_avail_mb=%d mem_tot_mb=%d\n",
            sample_idx, uptime_s, (unsigned long long)tot, (unsigned long long)idle,
            (unsigned long long)iow, mem_avail_mb, mem_tot_mb);

    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            if (!isdigit((unsigned char)ent->d_name[0])) continue;
            int pid = atoi(ent->d_name);
            if (pid <= 0) continue;
            char path[48];
            snprintf(path, sizeof(path), "/proc/%d/stat", pid);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char buf[512];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';
            char* lp = strchr(buf, '(');
            char* rp = strrchr(buf, ')');
            if (!lp || !rp || rp <= lp + 1) continue;
            std::string comm(lp + 1, (size_t)(rp - lp - 1));
            for (char& ch : comm) {
                if (ch == ' ' || ch == '\t') ch = '_';
            }
            if (comm.empty()) comm = "?";

            const char* rest = rp + 2;
            char state = 0;
            int ppid = 0;
            unsigned long long ut = 0, st = 0;
            int field = 0;
            const char* p = rest;
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                const char* tok = p;
                while (*p && *p != ' ') p++;
                field++;
                if (field == 1) state = *tok;
                else if (field == 2) ppid = atoi(tok);
                else if (field == 12) ut = strtoull(tok, nullptr, 10);
                else if (field == 13) { st = strtoull(tok, nullptr, 10); break; }
            }
            uint64_t ticks = (uint64_t)(ut + st);
            if (field >= 13 && (ticks > 0 || state == 'R' || state == 'D')) {
                fprintf(out, "%d\t%d\t%s\t%c\t%llu\n",
                        pid, ppid, comm.c_str(), state ? state : '?', (unsigned long long)ticks);
            }
        }
        closedir(dir);
    }
    fflush(out);
    (*proc_rows)++;
}

int run_cmd_capture(const char* const argv[], int timeout_sec, std::string* out_stdout, std::string* out_stderr) {
    int p_out[2]{-1, -1};
    int p_err[2]{-1, -1};
    if (pipe(p_out) != 0) return -1;
    if (pipe(p_err) != 0) {
        close(p_out[0]);
        close(p_out[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p_out[1], STDOUT_FILENO);
        dup2(p_err[1], STDERR_FILENO);
        close(p_out[0]); close(p_out[1]);
        close(p_err[0]); close(p_err[1]);
        execv(argv[0], (char* const*)argv);
        _exit(127);
    }
    close(p_out[1]);
    close(p_err[1]);
    if (pid < 0) {
        close(p_out[0]);
        close(p_err[0]);
        return -1;
    }

    fcntl(p_out[0], F_SETFL, O_NONBLOCK);
    fcntl(p_err[0], F_SETFL, O_NONBLOCK);
    out_stdout->clear();
    out_stderr->clear();
    int64_t deadline = boottime_ms() + (int64_t)timeout_sec * 1000LL;
    bool open_out = true, open_err = true;
    char buf[8192];
    while (open_out || open_err) {
        if (boottime_ms() > deadline) {
            kill(pid, SIGKILL);
            break;
        }
        bool any_read = false;
        if (open_out) {
            ssize_t n = read(p_out[0], buf, sizeof(buf));
            if (n > 0) {
                if (out_stdout->size() < 4 * 1024 * 1024) out_stdout->append(buf, (size_t)n);
                any_read = true;
            } else if (n == 0) {
                open_out = false;
            } else if (errno != EAGAIN && errno != EINTR) {
                open_out = false;
            }
        }
        if (open_err) {
            ssize_t n = read(p_err[0], buf, sizeof(buf));
            if (n > 0) {
                if (out_stderr->size() < 64 * 1024) out_stderr->append(buf, (size_t)n);
                any_read = true;
            } else if (n == 0) {
                open_err = false;
            } else if (errno != EAGAIN && errno != EINTR) {
                open_err = false;
            }
        }
        if (!any_read) usleep(10000);
    }
    close(p_out[0]);
    close(p_err[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 124;
}

void collect_boot_events(const std::string& run_dir, int* events_cnt, int* exit_code) {
    const char* argv[] = {
        "/system/bin/logcat", "-b", "events", "-d", "-v", "tag", "-s",
        "boot_progress_start:I", "boot_progress_preload_start:I", "boot_progress_preload_end:I",
        "boot_progress_system_run:I", "boot_progress_pms_start:I", "boot_progress_pms_system_scan_start:I",
        "boot_progress_pms_data_scan_start:I", "boot_progress_pms_scan_end:I", "boot_progress_pms_ready:I",
        "boot_progress_ams_ready:I", "boot_progress_enable_screen:I", "sf_stop_bootanim:I",
        "wm_boot_animation_done:I", nullptr
    };
    std::string out_s, err_s;
    *exit_code = run_cmd_capture(argv, 3, &out_s, &err_s);

    if (FILE* f = fopen((run_dir + "/logcat-exit.txt").c_str(), "w")) {
        fprintf(f, "%d\n", *exit_code);
        fclose(f);
    }
    if (FILE* f = fopen((run_dir + "/logcat-error.txt").c_str(), "w")) {
        fwrite(err_s.data(), 1, err_s.size(), f);
        fclose(f);
    }

    FILE* evf = fopen((run_dir + "/boot-events.tsv").c_str(), "w");
    if (!evf) return;
    int count = 0;
    size_t pos = 0;
    while (pos < out_s.size() && count < 128) {
        size_t nl = out_s.find('\n', pos);
        std::string line = (nl == std::string::npos) ? out_s.substr(pos) : out_s.substr(pos, nl - pos);
        char lvl = 0;
        char tag[96]{};
        char val[32]{};
        if (sscanf(line.c_str(), " %c/%95[^:]: %31s", &lvl, tag, val) == 3) {
            bool valid_tag = (strncmp(tag, "boot_progress_", 14) == 0
                || strcmp(tag, "sf_stop_bootanim") == 0
                || strcmp(tag, "wm_boot_animation_done") == 0);
            bool valid_num = true;
            for (int k = 0; val[k]; k++) {
                if (!isdigit((unsigned char)val[k])) { valid_num = false; break; }
            }
            if (valid_tag && valid_num && val[0] != '\0') {
                fprintf(evf, "%s\t%s\n", tag, val);
                count++;
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    fclose(evf);
    *events_cnt = count;
}

bool is_clean_token(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!(isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

void process_init_msg_line(
        double ts, const std::string& msg,
        std::unordered_set<std::string>& seen_act,
        FILE* initf, FILE* kpif, int* init_cnt) {
    unsigned long long kpi_v = 0;
    const char* kp = strstr(msg.c_str(), "KPI: ");
    if (kp && kpif) {
        if (sscanf(kp, "KPI: Bootloader start count = %llu", &kpi_v) == 1) {
            fprintf(kpif, "bl_start\t%llu\n", kpi_v);
        } else if (sscanf(kp, "KPI: Bootloader end count = %llu", &kpi_v) == 1) {
            fprintf(kpif, "bl_end\t%llu\n", kpi_v);
        } else if (sscanf(kp, "KPI: Bootloader load kernel count = %llu", &kpi_v) == 1) {
            fprintf(kpif, "bl_load_kernel\t%llu\n", kpi_v);
        } else if (sscanf(kp, "KPI: Kernel MPM timestamp = %llu", &kpi_v) == 1) {
            fprintf(kpif, "kernel_mpm\t%llu\n", kpi_v);
        } else if (sscanf(kp, "KPI: Kernel MPM Clock frequency = %llu", &kpi_v) == 1) {
            fprintf(kpif, "mpm_freq\t%llu\n", kpi_v);
        }
    }

    if (msg.rfind("init: ", 0) != 0 || *init_cnt >= 1024) return;
    std::string body = msg.substr(6);
    auto emit = [&](const char* kind, const std::string& name, const std::string& detail) {
        if (*init_cnt >= 1024) return;
        fprintf(initf, "%.6f\t%s\t%s\t%s\n", ts, kind, name.c_str(), detail.c_str());
        (*init_cnt)++;
    };

    unsigned long long nmod = 0, took_ms = 0;
    if (sscanf(body.c_str(), "Loaded %llu kernel modules took %llu ms", &nmod, &took_ms) == 2) {
        char db[64];
        snprintf(db, sizeof(db), "%llu_modules_%llums", nmod, took_ms);
        emit("phase", "kernel_modules", db);
    } else if (body.rfind("Switching root to '", 0) == 0 && body.back() == '\'') {
        std::string target = body.substr(19, body.size() - 20);
        if (target.size() > 1 && target[0] == '/' && is_clean_token(target.substr(1))) {
            emit("phase", "switch_root", target);
        }
    } else if (body == "init second stage started!") {
        emit("phase", "second_stage", "started");
    } else if (body.rfind("processing action (", 0) == 0) {
        size_t rp = body.find(") from ");
        if (rp != std::string::npos && rp > 19) {
            std::string act = body.substr(19, rp - 19);
            if (is_clean_token(act) && seen_act.insert(act).second) {
                emit("phase", act, "");
            }
        }
    } else if (body.rfind("starting service '", 0) == 0 && body.size() > 22
               && body.compare(body.size() - 4, 4, "'...") == 0) {
        std::string svc = body.substr(18, body.size() - 22);
        if (is_clean_token(svc)) emit("start", svc, "");
    } else if (body.rfind("Service '", 0) == 0) {
        size_t q2 = body.find("' (pid ", 9);
        const char* ex = strstr(body.c_str(), " exited with status ");
        const char* tk = strstr(body.c_str(), " took ");
        if (q2 != std::string::npos && ex && tk) {
            std::string svc = body.substr(9, q2 - 9);
            int st_val = -1;
            double tk_s = -1.0;
            if (is_clean_token(svc)
                    && sscanf(ex, " exited with status %d", &st_val) == 1
                    && sscanf(tk, " took %lf seconds", &tk_s) == 1
                    && st_val >= 0 && tk_s >= 0.0) {
                char db[64];
                snprintf(db, sizeof(db), "status=%d;took=%.6fs", st_val, tk_s);
                emit("exit", svc, db);
            }
        }
    }
}

void collect_kmsg_and_init(const std::string& run_dir, int* init_cnt, int* dmesg_exit) {
    FILE* initf = fopen((run_dir + "/init-events.tsv").c_str(), "w");
    FILE* kpif = fopen((run_dir + "/bootloader-kpi.tsv").c_str(), "w");
    FILE* errf = fopen((run_dir + "/dmesg-error.txt").c_str(), "w");
    if (!initf) return;

    std::unordered_set<std::string> seen_act;
    int cnt = 0;
    int rc = 0;

    int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kfd >= 0) {
        char buf[4096];
        for (;;) {
            ssize_t n = read(kfd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                const char* semi = strchr(buf, ';');
                unsigned long prio = 0;
                unsigned long long seq = 0, ts_us = 0;
                if (semi && sscanf(buf, "%lu,%llu,%llu", &prio, &seq, &ts_us) == 3) {
                    const char* eol = strpbrk(semi + 1, "\r\n");
                    std::string msg = eol ? std::string(semi + 1, (size_t)(eol - (semi + 1))) : std::string(semi + 1);
                    process_init_msg_line((double)ts_us / 1e6, msg, seen_act, initf, kpif, &cnt);
                }
                continue;
            }
            if (n < 0 && (errno == EPIPE || errno == EINTR)) continue;
            break;
        }
        close(kfd);
    } else {
        const char* argv[] = {"/system/bin/dmesg", nullptr};
        std::string out_s, err_s;
        rc = run_cmd_capture(argv, 3, &out_s, &err_s);
        if (errf && !err_s.empty()) fwrite(err_s.data(), 1, err_s.size(), errf);
        size_t pos = 0;
        while (pos < out_s.size()) {
            size_t nl = out_s.find('\n', pos);
            std::string line = (nl == std::string::npos) ? out_s.substr(pos) : out_s.substr(pos, nl - pos);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            double ts = 0.0;
            size_t rb = line.find(']');
            if (!line.empty() && line[0] == '[' && rb != std::string::npos && sscanf(line.c_str() + 1, "%lf", &ts) == 1) {
                std::string msg = trim_copy(line.substr(rb + 1));
                process_init_msg_line(ts, msg, seen_act, initf, kpif, &cnt);
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    fclose(initf);
    if (kpif) fclose(kpif);
    if (errf) fclose(errf);
    if (FILE* f = fopen((run_dir + "/dmesg-exit.txt").c_str(), "w")) {
        fprintf(f, "%d\n", rc);
        fclose(f);
    }
    *init_cnt = cnt;
    *dmesg_exit = rc;
}

void copy_sanitized_uefi_log(const char* src_path, const std::string& dst_path) {
    FILE* sf = fopen(src_path, "rb");
    if (!sf) return;
    std::string raw(131072, '\0');
    size_t n = fread(&raw[0], 1, raw.size(), sf);
    fclose(sf);
    raw.resize(n);
    for (char& ch : raw) {
        if (ch == '\0') ch = '\n';
    }
    FILE* df = fopen(dst_path.c_str(), "w");
    if (!df) return;
    int lines = 0;
    size_t pos = 0;
    while (pos < raw.size() && lines < 700) {
        size_t nl = raw.find('\n', pos);
        std::string line = (nl == std::string::npos) ? raw.substr(pos) : raw.substr(pos, nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (!line.empty()) {
            fwrite(line.data(), 1, line.size(), df);
            fputc('\n', df);
            lines++;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    fclose(df);
}

void copy_head_lines(const char* src_path, const std::string& dst_path, int max_lines) {
    FILE* sf = fopen(src_path, "r");
    if (!sf) return;
    FILE* df = fopen(dst_path.c_str(), "w");
    if (!df) {
        fclose(sf);
        return;
    }
    char buf[1024];
    int count = 0;
    while (count < max_lines && fgets(buf, sizeof(buf), sf)) {
        fputs(buf, df);
        count++;
    }
    fclose(df);
    fclose(sf);
}

}  // namespace

int prune_old_runs(
        int keep, const std::string& protect_boot_id, int* kept_out, int* deleted_out) {
    if (kept_out) *kept_out = 0;
    if (deleted_out) *deleted_out = 0;
    if (keep < 0) return 1;
    auto runs = list_run_dirs_sorted();
    int kept = 0;
    int deleted = 0;
    for (const auto& r : runs) {
        if ((!protect_boot_id.empty() && r.boot_id == protect_boot_id) || kept < keep) {
            kept++;
        } else {
            if (remove_dir_recursive(r.path)) deleted++;
        }
    }
    if (kept_out) *kept_out = kept;
    if (deleted_out) *deleted_out = deleted;
    return 0;
}

int run_collector(const CollectOptions& opts) {
    umask(077);
    std::string boot_id;
    std::string run_dir;

    if (!opts.out_dir.empty()) {
        run_dir = opts.out_dir;
        if (!mkdir_p(run_dir, 0700)) return 1;
        boot_id = "manual-smoke";
    } else {
        FILE* bf = fopen("/proc/sys/kernel/random/boot_id", "r");
        if (!bf) return 1;
        char bid[64]{};
        if (fgets(bid, sizeof(bid), bf)) boot_id = trim_copy(bid);
        fclose(bf);
        if (!is_valid_uuid(boot_id)) return 1;

        if (!mkdir_p(kBaseDir, 0700)) return 1;
        chmod(kBaseDir, 0700);
        run_dir = std::string(kBaseDir) + "/" + boot_id;
        if (!mkdir_p(run_dir, 0700)) return 1;
        chmod(run_dir.c_str(), 0700);
        if (mkdir((run_dir + "/.probe.lock").c_str(), 0700) != 0) {
            return (errno == EEXIST) ? 0 : 1;
        }
    }

    std::string meta_path = run_dir + "/metadata.txt";
    if (FILE* mf = fopen(meta_path.c_str(), "w")) {
        fprintf(mf, "format=4\n");
        fprintf(mf, "boot_id=%s\n", boot_id.c_str());
        fprintf(mf, "started_uptime=%.2f\n", read_uptime_sec());
        fprintf(mf, "android_version=%s\n", get_prop("ro.build.version.release").c_str());
        fprintf(mf, "sdk=%s\n", get_prop("ro.build.version.sdk").c_str());
        fprintf(mf, "model=%s\n", get_prop("ro.product.model").c_str());
        fprintf(mf, "bootreason=%s\n", get_prop("ro.boot.bootreason").c_str());
        fprintf(mf, "slot_suffix=%s\n", get_prop("ro.boot.slot_suffix").c_str());
        fclose(mf);
    }

    FILE* samples_f = fopen((run_dir + "/samples.txt").c_str(), "w");
    FILE* psi_f = fopen((run_dir + "/psi.tsv").c_str(), "w");
    FILE* proc_f = fopen((run_dir + "/proc.tsv").c_str(), "w");
    if (!samples_f) {
        if (psi_f) fclose(psi_f);
        if (proc_f) fclose(proc_f);
        return 1;
    }

    int count = 0;
    int complete_count = 0;
    int psi_samples = 0;
    int proc_samples = 0;
    while (count < 60) {
        double up = read_uptime_sec();
        write_props_sample(samples_f, count, up);
        if (psi_f) write_psi_row(psi_f, count, up, &psi_samples);
        if (proc_f) write_proc_snapshot(proc_f, count, up, &proc_samples);
        count++;
        if (opts.once) break;
        if (get_prop("sys.boot_completed") == "1") {
            complete_count++;
            if (complete_count >= 3) break;
        } else {
            complete_count = 0;
        }
        sleep(1);
    }
    fclose(samples_f);
    if (psi_f) fclose(psi_f);
    if (proc_f) fclose(proc_f);

    int events_count = 0, logcat_exit = 0;
    collect_boot_events(run_dir, &events_count, &logcat_exit);

    int init_count = 0, dmesg_exit = 0;
    collect_kmsg_and_init(run_dir, &init_count, &dmesg_exit);

    copy_sanitized_uefi_log("/dev/logfs/UefiLog0.txt", run_dir + "/bootloader-uefi.txt");
    copy_head_lines("/proc/bootprof", run_dir + "/bootloader-bootprof.txt", 120);
    copy_head_lines("/sys/kernel/boot_kpi/kpi_values", run_dir + "/bootloader-sysfs-kpi.txt", 120);

    if (FILE* mf = fopen(meta_path.c_str(), "a")) {
        fprintf(mf, "finished_uptime=%.2f\n", read_uptime_sec());
        fprintf(mf, "samples=%d\n", count);
        fprintf(mf, "boot_completed=%s\n", get_prop("sys.boot_completed").c_str());
        fprintf(mf, "events_logcat_exit=%d\n", logcat_exit);
        fprintf(mf, "events_count=%d\n", events_count);
        fprintf(mf, "dmesg_exit=%d\n", dmesg_exit);
        fprintf(mf, "init_events_count=%d\n", init_count);
        fprintf(mf, "psi_samples=%d\n", psi_samples);
        fprintf(mf, "proc_samples=%d\n", proc_samples);
        fclose(mf);
    }

    if (opts.out_dir.empty()) {
        int keep = opts.keep_history >= 0 ? opts.keep_history : read_config_int("max_history", 20);
        if (keep > 0) {
            prune_old_runs(keep, boot_id, nullptr, nullptr);
        }
    }
    return 0;
}

}  // namespace bootscope
