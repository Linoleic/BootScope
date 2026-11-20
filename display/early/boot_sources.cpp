#include "early/boot_sources.h"
#include "early/common.h"
#include "early/early_trace.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>

namespace bootscope {

bool map_framework_stage(const char* tag, const char* ms, StageInfo* out) {
    struct Entry {
        const char* tag;
        int step;
        const char* label;
    };
    static const Entry kMap[] = {
        {"boot_progress_start", 4, "BOOT_START"},
        {"boot_progress_preload_start", 5, "PRELOAD"},
        {"boot_progress_preload_end", 6, "PRELOADED"},
        {"boot_progress_system_run", 7, "SYSTEM_RUN"},
        {"boot_progress_pms_start", 8, "PMS_START"},
        {"boot_progress_pms_system_scan_start", 8, "PMS_SYS_SCAN"},
        {"boot_progress_pms_data_scan_start", 8, "PMS_DATA_SCAN"},
        {"boot_progress_pms_scan_end", 8, "PMS_SCANNED"},
        {"boot_progress_pms_ready", 8, "PMS_READY"},
        {"boot_progress_ams_ready", 9, "AMS_READY"},
        {"boot_progress_enable_screen", 10, "ENABLE_SCREEN"},
        {"sf_stop_bootanim", 10, "STOP_BOOTANIM"},
        {"wm_boot_animation_done", 10, "ANIM_DONE"},
    };
    for (const auto& e : kMap) {
        if (strcmp(tag, e.tag) == 0) {
            out->step = e.step;
            out->label = e.label;
            out->raw_ms = ms;
            return true;
        }
    }
    return false;
}

StageInfo infer_native_stage(const std::string& apex_state, const std::string& zyg_state) {
    std::string apexd_status = get_prop("apexd.status");
    if (zyg_state == "RUN") {
        return {3, "ZYGOTE_START", ""};
    }
    if (apexd_status == "ready" || apexd_status == "activated" || apex_state == "RUN" || apex_state == "STOP") {
        return {2, "APEX_POST_FS", ""};
    }
    return {1, "EARLY_BOOTANIM", ""};
}

void push_tail(
        EventState* st, int64_t ts_ms, const char* source, TailKind kind,
        const std::string& raw_text, const std::string& group) {
    std::string text = sanitize_tail_text(st->rules, raw_text, kTailLineChars - 8);
    if (text.empty()) return;

    const char* kind_str = "INFO";
    if (kind == TailKind::Stage) kind_str = "STAGE";
    else if (kind == TailKind::Error) kind_str = "ERR";
    else if (kind == TailKind::ModuleError) kind_str = "MERR";

    std::lock_guard<std::mutex> lock(st->mu);
    for (auto& existing : st->tail) {
        bool same_msg = (existing.source == source && existing.text == text);
        bool same_err_tag = (!group.empty() && existing.source == source && existing.group == group);
        if (same_msg || same_err_tag) {
            bool emit_trace = (kind != TailKind::Stage) && (ts_ms - existing.ts_ms >= 2000);
            if (ts_ms >= existing.ts_ms) {
                if (emit_trace) trace_tail(ts_ms, source, kind_str, text);
                existing.ts_ms = ts_ms;
                existing.text = std::move(text);
                existing.kind = kind;
                existing.seq = st->next_seq++;
            }
            std::sort(st->tail.begin(), st->tail.end(), [](const TailEntry& a, const TailEntry& b) {
                if (a.ts_ms != b.ts_ms) return a.ts_ms < b.ts_ms;
                return a.seq < b.seq;
            });
            return;
        }
    }
    trace_tail(ts_ms, source, kind_str, text);
    TailEntry entry;
    entry.ts_ms = ts_ms;
    entry.seq = st->next_seq++;
    entry.source = source;
    entry.group = group;
    entry.text = std::move(text);
    entry.kind = kind;
    st->tail.push_back(std::move(entry));
    std::sort(st->tail.begin(), st->tail.end(), [](const TailEntry& a, const TailEntry& b) {
        if (a.ts_ms != b.ts_ms) return a.ts_ms < b.ts_ms;
        return a.seq < b.seq;
    });
    constexpr size_t kMaxBufferedTail = 16;
    if (st->tail.size() > kMaxBufferedTail) {
        st->tail.erase(st->tail.begin(), st->tail.begin() + (st->tail.size() - kMaxBufferedTail));
    }
}

static bool extract_quoted_token(const char* src, std::string* out) {
    const char* q1 = strchr(src, '\'');
    if (!q1) return false;
    const char* q2 = strchr(q1 + 1, '\'');
    if (!q2 || q2 <= q1 + 1) return false;
    out->assign(q1 + 1, (size_t)(q2 - q1 - 1));
    return true;
}

static bool is_major_init_action(const std::string& action) {
    static const char* const kMajor[] = {
        "early-init", "init", "late-fs", "post-fs", "post-fs-data",
        "load_persist_props_action", "zygote-start", "firmware_mounts_complete", "boot",
    };
    for (const char* m : kMajor) {
        if (action == m) return true;
    }
    return false;
}

static bool is_resident_daemon(const std::string& svc) {
    if (svc.rfind("vendor.", 0) == 0 || svc.rfind("android.hardware.", 0) == 0) return true;
    static const char* const kDaemons[] = {
        "ueventd", "logd", "servicemanager", "hwservicemanager", "vndservicemanager",
        "vold", "netd", "surfaceflinger", "bootanim", "bootscope_early",
        "zygote", "zygote64", "webview_zygote", "adbd", "lmkd", "tombstoned",
        "traced", "traced_probes", "statsd", "keystore2", "credstore",
        "incidentd", "storaged", "audioserver", "cameraserver", "mediaserver",
        "drmserver", "thermalservice", "apexd", "artd", "wpa_supplicant",
    };
    for (const char* d : kDaemons) {
        if (svc == d) return true;
    }
    return false;
}

static bool is_module_error_hint(const RuleSet& rules, const std::string& raw) {
    if (!matches_module_key(rules, raw)) return false;
    static const char* const kErrWords[] = {
        "denied", "permission", "error", "fail", "abort",
        "segfault", "cannot", "not found", "unable", "sigabrt", "sigsegv",
    };
    for (const char* w : kErrWords) {
        if (icontains(raw, w)) return true;
    }
    return false;
}

static void update_qcom_kpi_locked(EventState* st) {
    const std::string& cur = st->phases[0].detail;
    if (cur != "BOOTLOADER TIMING N/A" && cur.rfind("QCOM KPI", 0) != 0) {
        return;
    }
    const QcomKpi& k = st->qcom_kpi;
    if (!k.has_start || !k.has_end || k.bl_end < k.bl_start) return;
    uint64_t total_ticks = k.bl_end - k.bl_start;
    if (k.has_freq && k.mpm_freq > 0) {
        int64_t total_ms = (int64_t)llround(((double)total_ticks * 1000.0) / (double)k.mpm_freq);
        st->phases[0].start_ms = 0;
        st->phases[0].end_ms = total_ms;
        char buf[64];
        if (k.has_load && k.bl_load_kernel >= k.bl_start && k.bl_load_kernel <= k.bl_end) {
            int64_t pre_ms = (int64_t)llround(((double)(k.bl_load_kernel - k.bl_start) * 1000.0) / (double)k.mpm_freq);
            int64_t kl_ms = (int64_t)llround(((double)(k.bl_end - k.bl_load_kernel) * 1000.0) / (double)k.mpm_freq);
            snprintf(buf, sizeof(buf), "QCOM KPI PRE:%lldMS KL:%lldMS",
                     (long long)pre_ms, (long long)kl_ms);
        } else {
            snprintf(buf, sizeof(buf), "QCOM KPI BL:%lldMS F:%llu",
                     (long long)total_ms, (unsigned long long)k.mpm_freq);
        }
        st->phases[0].detail = buf;
        trace_bl(0, buf, total_ms);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "QCOM KPI TICKS:%llu", (unsigned long long)total_ticks);
        st->phases[0].detail = buf;
    }
}

static void process_kmsg_record(EventState* st, const char* rec) {
    const char* semi = strchr(rec, ';');
    if (!semi) return;

    unsigned long prio = 6;
    unsigned long long seq = 0;
    unsigned long long ts_us = 0;
    if (sscanf(rec, "%lu,%llu,%llu", &prio, &seq, &ts_us) < 3) return;
    int lvl = (int)(prio & 7UL);
    int64_t ts_ms = (int64_t)(ts_us / 1000ULL);

    const char* msg_start = semi + 1;
    const char* eol = strpbrk(msg_start, "\r\n");
    std::string msg = eol ? std::string(msg_start, (size_t)(eol - msg_start)) : std::string(msg_start);

    if (msg.rfind("init: ", 0) == 0) {
        const char* body = msg.c_str() + 6;
        if (strncmp(body, "starting service '", 18) == 0) {
            std::string svc;
            if (extract_quoted_token(body + 17, &svc)) {
                if (!is_resident_daemon(svc)) {
                    std::lock_guard<std::mutex> lock(st->mu);
                    bool found = false;
                    for (auto& a : st->active_svcs) {
                        if (a.name == svc) {
                            a.start_ms = ts_ms;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        if (st->active_svcs.size() >= 32) st->active_svcs.erase(st->active_svcs.begin());
                        st->active_svcs.push_back({svc, ts_ms});
                    }
                }
                push_tail(st, ts_ms, "K", TailKind::Info, "+" + svc);
            }
            return;
        }
        if (strncmp(body, "Service '", 9) == 0) {
            std::string svc;
            if (extract_quoted_token(body + 8, &svc)) {
                double took_s = 0.0;
                const char* took_p = strstr(body, "took ");
                if (took_p) sscanf(took_p, "took %lf", &took_s);
                {
                    std::lock_guard<std::mutex> lock(st->mu);
                    st->active_svcs.erase(
                        std::remove_if(st->active_svcs.begin(), st->active_svcs.end(),
                                       [&](const ActiveService& a) { return a.name == svc; }),
                        st->active_svcs.end());
                    int64_t dur_ms = (int64_t)llround(took_s * 1000.0);
                    bool in_init = (st->phases[1].state == NodeState::Running)
                        || (st->phases[1].end_ms > 0 && ts_ms <= st->phases[1].end_ms + 500);
                    if (in_init && dur_ms > st->max_init_dur_ms && !is_resident_daemon(svc)) {
                        std::string clean_svc = svc;
                        if (clean_svc.rfind("exec ", 0) == 0) {
                            size_t lp = clean_svc.find('(');
                            size_t rp = clean_svc.rfind(')');
                            if (lp != std::string::npos && rp != std::string::npos && rp > lp + 1) {
                                clean_svc = clean_svc.substr(lp + 1, rp - lp - 1);
                                size_t sp = clean_svc.find(' ');
                                if (sp != std::string::npos) clean_svc.resize(sp);
                                size_t sl = clean_svc.rfind('/');
                                if (sl != std::string::npos) clean_svc = clean_svc.substr(sl + 1);
                            }
                        }
                        st->max_init_dur_ms = dur_ms;
                        st->max_init_svc = std::move(clean_svc);
                    }
                }

                bool is_self = matches_module_key(st->rules, svc);
                const char* killed = strstr(body, "killed by signal ");
                if (killed) {
                    int sig = 0;
                    sscanf(killed, "killed by signal %d", &sig);
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s%s SIG=%d", is_self ? "ERR " : "", svc.c_str(), sig);
                    push_tail(st, ts_ms, is_self ? "M" : "E",
                              is_self ? TailKind::ModuleError : TailKind::Error,
                              buf, is_self ? ("MOD:" + svc) : "");
                    return;
                }
                const char* status_p = strstr(body, "exited with status ");
                if (status_p) {
                    int rc = 0;
                    sscanf(status_p, "exited with status %d", &rc);
                    if (rc != 0) {
                        if (!is_self && matches_ignore_svc(st->rules, svc)) return;
                        char buf[64];
                        snprintf(buf, sizeof(buf), "%s%s RC=%d", is_self ? "ERR " : "", svc.c_str(), rc);
                        push_tail(st, ts_ms, is_self ? "M" : "E",
                                  is_self ? TailKind::ModuleError : TailKind::Error,
                                  buf, is_self ? ("MOD:" + svc) : "");
                    } else if (took_s >= 0.10) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "-%s %.2fS", svc.c_str(), took_s);
                        push_tail(st, ts_ms, "K", TailKind::Info, buf);
                    }
                }
            }
            return;
        }
        if (strncmp(body, "Command '", 9) == 0) {
            std::string full_cmd;
            if (extract_quoted_token(body + 8, &full_cmd)) {
                bool is_self = matches_module_key(st->rules, body);
                std::string short_cmd = full_cmd;
                size_t sp = short_cmd.find(' ');
                if (sp != std::string::npos) short_cmd.resize(sp);
                if (strstr(body, "and failed:") != nullptr) {
                    if (!is_self && matches_ignore_cmd(st->rules, full_cmd)) return;
                    push_tail(st, ts_ms, is_self ? "M" : "E",
                              is_self ? TailKind::ModuleError : TailKind::Error,
                              (is_self ? "ERR CMD " : "CMD FAIL ") + short_cmd,
                              is_self ? "MOD:CMD" : "");
                } else {
                    const char* took_p = strstr(body, "took ");
                    int took_ms = 0;
                    if (took_p && sscanf(took_p, "took %dms", &took_ms) == 1 && took_ms >= 100) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "CMD %s %dMS", short_cmd.c_str(), took_ms);
                        push_tail(st, ts_ms, "K", TailKind::Info, buf);
                    }
                }
            }
            return;
        }
        if (strncmp(body, "processing action (", 19) == 0) {
            const char* start = body + 19;
            const char* end = strchr(start, ')');
            if (end && end > start) {
                std::string act(start, (size_t)(end - start));
                size_t amp = act.rfind("&&");
                if (amp != std::string::npos) {
                    act = act.substr(amp + 2);
                    size_t first_ns = act.find_first_not_of(' ');
                    if (first_ns != std::string::npos) act = act.substr(first_ns);
                }
                if (is_major_init_action(act)) {
                    {
                        std::lock_guard<std::mutex> lock(st->mu);
                        st->last_init_action = act;
                        bool seen = false;
                        for (const auto& m : st->init_milestones) {
                            if (m.first == act) {
                                seen = true;
                                break;
                            }
                        }
                        if (!seen && st->init_milestones.size() < 8) {
                            st->init_milestones.emplace_back(act, ts_ms);
                        }
                    }
                    push_tail(st, ts_ms, "K", TailKind::Stage, "ACT " + act);
                }
            }
        }
        return;
    }

    if (matches_module_key(st->rules, msg) && (icontains(msg, "selinux") || icontains(msg, "avc:"))) {
        push_tail(st, ts_ms, "M", TailKind::ModuleError, "ERR SELINUX DENY BOOTSCOPE", "MOD:SELINUX");
        return;
    }
    const char* kpi_p = strstr(msg.c_str(), "KPI: ");
    if (kpi_p) {
        unsigned long long val = 0;
        bool updated = false;
        std::lock_guard<std::mutex> lock(st->mu);
        if (sscanf(kpi_p, "KPI: Bootloader start count = %llu", &val) == 1) {
            st->qcom_kpi.bl_start = (uint64_t)val;
            st->qcom_kpi.has_start = true;
            updated = true;
        } else if (sscanf(kpi_p, "KPI: Bootloader end count = %llu", &val) == 1) {
            st->qcom_kpi.bl_end = (uint64_t)val;
            st->qcom_kpi.has_end = true;
            updated = true;
        } else if (sscanf(kpi_p, "KPI: Bootloader load kernel count = %llu", &val) == 1) {
            st->qcom_kpi.bl_load_kernel = (uint64_t)val;
            st->qcom_kpi.has_load = true;
            updated = true;
        } else if (sscanf(kpi_p, "KPI: Kernel MPM timestamp = %llu", &val) == 1) {
            st->qcom_kpi.kernel_mpm = (uint64_t)val;
            st->qcom_kpi.has_kernel = true;
            updated = true;
        } else if (sscanf(kpi_p, "KPI: Kernel MPM Clock frequency = %llu", &val) == 1) {
            st->qcom_kpi.mpm_freq = (uint64_t)val;
            st->qcom_kpi.has_freq = true;
            updated = true;
        }
        if (updated) {
            update_qcom_kpi_locked(st);
            return;
        }
    }
    if (is_module_error_hint(st->rules, msg)) {
        push_tail(st, ts_ms, "M", TailKind::ModuleError, "ERR " + msg, "MOD:KMSG");
        return;
    }

    bool crit_kernel = (lvl <= 2);
    bool severe_err = (lvl == 3) && (
        icontains(msg, "panic") || icontains(msg, "oops:")
        || icontains(msg, "bug:") || icontains(msg, "watchdog")
        || icontains(msg, "ext4-fs error") || icontains(msg, "f2fs-fs error")
        || icontains(msg, "i/o error"));
    if (crit_kernel || severe_err) {
        push_tail(st, ts_ms, "E", TailKind::Error, msg);
    }
}

void* kmsg_reader_thread(void* arg) {
    EventState* st = (EventState*)arg;
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ERR KMSG OPEN=%d", errno);
        push_tail(st, boottime_ms(), "M", TailKind::ModuleError, buf, "MOD:KMSG_OPEN");
        return nullptr;
    }

    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    char buf[4096];
    while (!st->stop) {
        for (;;) {
            if (st->stop) break;
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                process_kmsg_record(st, buf);
                continue;
            }
            if (n < 0 && (errno == EPIPE || errno == EINTR)) {
                continue;
            }
            break;
        }
        if (st->stop) break;
        poll(&pfd, 1, 200);
    }
    close(fd);
    return nullptr;
}

bool is_stack_trace_noise(const char* msg) {
    if (!msg || !*msg) return true;
    while (*msg == ' ' || *msg == '\t') msg++;
    if (!*msg) return true;
    if (strncmp(msg, "at ", 3) == 0) return true;
    if (strncmp(msg, "Caused by:", 10) == 0) return true;
    if (strncmp(msg, "... ", 4) == 0) return true;
    if (strncmp(msg, "Suppressed:", 11) == 0) return true;
    if (msg[0] == '#' && isdigit((unsigned char)msg[1])) return true;
    return false;
}

void* err_reader_thread(void* arg) {
    EventState* st = (EventState*)arg;
    for (int attempt = 0; attempt < 6 && !st->stop; attempt++) {
        int pipefd[2]{-1, -1};
        if (pipe(pipefd) != 0) break;

        pid_t pid = fork();
        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            const char* argv[] = {
                "/system/bin/logcat", "-b", "main,system,crash", "-v", "monotonic", "*:E", nullptr
            };
            execv(argv[0], (char* const*)argv);
            _exit(127);
        }
        close(pipefd[1]);
        if (pid < 0) {
            close(pipefd[0]);
            usleep(300000);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(st->mu);
            st->err_logcat_pid = pid;
        }

        FILE* fp = fdopen(pipefd[0], "r");
        if (fp) {
            char line[512];
            while (!st->stop && fgets(line, sizeof(line), fp)) {
                double sec = 0.0;
                int lpid = 0, ltid = 0;
                char lvl = 0;
                char tag[64]{};
                int tag_end = 0;
                if (sscanf(line, " %lf %d %d %c %63[^:]:%n", &sec, &lpid, &ltid, &lvl, tag, &tag_end) == 5
                        && tag_end > 0) {
                    const char* msg = line + tag_end;
                    if (is_stack_trace_noise(msg)) continue;
                    std::string tag_str(tag);
                    while (!tag_str.empty() && tag_str.back() == ' ') tag_str.pop_back();
                    std::string full = tag_str + ": " + msg;
                    int64_t ts_ms = (sec < 0.0 || sec > 86400.0)
                        ? boottime_ms()
                        : (int64_t)llround(sec * 1000.0);
                    if (matches_module_key(st->rules, tag_str) || matches_module_key(st->rules, msg)) {
                        push_tail(st, ts_ms, "M", TailKind::ModuleError, "ERR " + full, "MOD:" + tag_str);
                    } else if (!matches_ignore_tag(st->rules, tag_str)) {
                        const char* src = (lvl == 'F') ? "F" : "E";
                        push_tail(st, ts_ms, src, TailKind::Error, full, tag_str);
                    }
                }
            }
            fclose(fp);
        } else {
            close(pipefd[0]);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        {
            std::lock_guard<std::mutex> lock(st->mu);
            st->err_logcat_pid = -1;
        }
        if (!st->stop && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "ERR LOGCAT-ERR RC=%d", WEXITSTATUS(status));
            push_tail(st, boottime_ms(), "M", TailKind::ModuleError, buf, "MOD:LOGCAT_ERR");
        }
        if (!st->stop) usleep(400000);
    }
    return nullptr;
}

void* event_reader_thread(void* arg) {
    EventState* st = (EventState*)arg;
    for (int attempt = 0; attempt < 8 && !st->stop; attempt++) {
        int pipefd[2]{-1, -1};
        if (pipe(pipefd) != 0) break;

        pid_t pid = fork();
        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            const char* argv[] = {
                "/system/bin/logcat", "-b", "events", "-v", "tag", "-s",
                "boot_progress_start:I",
                "boot_progress_preload_start:I",
                "boot_progress_preload_end:I",
                "boot_progress_system_run:I",
                "boot_progress_pms_start:I",
                "boot_progress_pms_system_scan_start:I",
                "boot_progress_pms_data_scan_start:I",
                "boot_progress_pms_scan_end:I",
                "boot_progress_pms_ready:I",
                "boot_progress_ams_ready:I",
                "boot_progress_enable_screen:I",
                "sf_stop_bootanim:I",
                "wm_boot_animation_done:I",
                nullptr
            };
            execv(argv[0], (char* const*)argv);
            _exit(127);
        }
        close(pipefd[1]);
        if (pid < 0) {
            close(pipefd[0]);
            usleep(300000);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(st->mu);
            st->logcat_pid = pid;
        }

        FILE* fp = fdopen(pipefd[0], "r");
        if (fp) {
            char line[256];
            while (!st->stop && fgets(line, sizeof(line), fp)) {
                char tag[96]{};
                char ms[32]{};
                if (sscanf(line, "%*c/%95[^:]: %31[0-9]", tag, ms) == 2) {
                    StageInfo next;
                    if (map_framework_stage(tag, ms, &next)) {
                        int64_t ts_ms = strtoll(ms, nullptr, 10);
                        if (ts_ms <= 0) ts_ms = boottime_ms();
                        {
                            std::lock_guard<std::mutex> lock(st->mu);
                            st->framework_stage = next;
                            if (strcmp(tag, "boot_progress_start") == 0) {
                                if (st->phases[1].state != NodeState::Done) {
                                    st->phases[1].state = NodeState::Done;
                                    if (st->phases[1].end_ms < 0) st->phases[1].end_ms = ts_ms;
                                }
                                if (st->phases[2].start_ms < 0) st->phases[2].start_ms = 0;
                                st->phases[2].state = NodeState::Done;
                                st->phases[2].end_ms = ts_ms;
                                st->phases[2].detail = "SYSTEM_SERVER STARTED";
                                if (st->phases[3].state == NodeState::Pending) {
                                    st->phases[3].state = NodeState::Running;
                                    st->phases[3].start_ms = ts_ms;
                                    st->phases[3].detail = "WAITING PRELOAD";
                                }
                            } else if (strcmp(tag, "boot_progress_preload_start") == 0) {
                                st->phases[3].state = NodeState::Running;
                                st->phases[3].start_ms = ts_ms;
                                st->phases[3].detail = "PRELOADING CLASSES";
                            } else if (strcmp(tag, "boot_progress_preload_end") == 0) {
                                if (st->phases[3].start_ms < 0) st->phases[3].start_ms = ts_ms;
                                st->phases[3].state = NodeState::Done;
                                st->phases[3].end_ms = ts_ms;
                                st->phases[3].detail = "PRELOADED";
                                if (st->phases[4].state == NodeState::Pending) {
                                    st->phases[4].state = NodeState::Running;
                                    st->phases[4].start_ms = ts_ms;
                                    st->phases[4].detail = "WAITING SYSTEM_RUN";
                                }
                            } else if (strcmp(tag, "boot_progress_system_run") == 0) {
                                if (st->phases[3].state != NodeState::Done) {
                                    if (st->phases[3].start_ms < 0) st->phases[3].start_ms = ts_ms;
                                    st->phases[3].state = NodeState::Done;
                                    st->phases[3].end_ms = ts_ms;
                                }
                                st->phases[4].state = NodeState::Running;
                                if (st->phases[4].start_ms < 0) st->phases[4].start_ms = ts_ms;
                                st->phases[4].detail = "SYSTEM_RUN";
                            } else if (strcmp(tag, "boot_progress_pms_start") == 0) {
                                st->phases[4].state = NodeState::Running;
                                if (st->phases[4].start_ms < 0) st->phases[4].start_ms = ts_ms;
                                st->phases[4].detail = "PMS_START";
                            } else if (strcmp(tag, "boot_progress_pms_system_scan_start") == 0) {
                                st->phases[4].state = NodeState::Running;
                                if (st->phases[4].start_ms < 0) st->phases[4].start_ms = ts_ms;
                                st->phases[4].detail = "SYSTEM_PKG_SCAN";
                            } else if (strcmp(tag, "boot_progress_pms_data_scan_start") == 0) {
                                st->phases[4].state = NodeState::Running;
                                if (st->phases[4].start_ms < 0) st->phases[4].start_ms = ts_ms;
                                st->phases[4].detail = "DATA_PKG_SCAN";
                            } else if (strcmp(tag, "boot_progress_pms_scan_end") == 0) {
                                st->phases[4].detail = "SCAN_END";
                            } else if (strcmp(tag, "boot_progress_pms_ready") == 0) {
                                if (st->phases[4].start_ms < 0) st->phases[4].start_ms = ts_ms;
                                st->phases[4].state = NodeState::Done;
                                st->phases[4].end_ms = ts_ms;
                                st->phases[4].detail = "PMS_READY";
                                if (st->phases[5].state == NodeState::Pending) {
                                    st->phases[5].state = NodeState::Running;
                                    st->phases[5].start_ms = ts_ms;
                                    st->phases[5].detail = "WAITING AMS_READY";
                                }
                            } else if (strcmp(tag, "boot_progress_ams_ready") == 0) {
                                if (st->phases[4].state != NodeState::Done) {
                                    if (st->phases[4].start_ms < 0) st->phases[4].start_ms = ts_ms;
                                    st->phases[4].state = NodeState::Done;
                                    st->phases[4].end_ms = ts_ms;
                                }
                                st->phases[5].state = NodeState::Running;
                                if (st->phases[5].start_ms < 0) st->phases[5].start_ms = ts_ms;
                                st->phases[5].detail = "AMS_READY -> SCREEN";
                            } else if (strcmp(tag, "boot_progress_enable_screen") == 0
                                    || strcmp(tag, "sf_stop_bootanim") == 0
                                    || strcmp(tag, "wm_boot_animation_done") == 0) {
                                if (st->phases[5].start_ms < 0) st->phases[5].start_ms = ts_ms;
                                st->phases[5].state = NodeState::Done;
                                st->phases[5].end_ms = ts_ms;
                                st->phases[5].detail = next.label;
                            }
                        }
                        push_tail(st, ts_ms, "P", TailKind::Stage, next.label);
                    }
                }
            }
            fclose(fp);
        } else {
            close(pipefd[0]);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        {
            std::lock_guard<std::mutex> lock(st->mu);
            st->logcat_pid = -1;
        }
        if (!st->stop && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "ERR LOGCAT-EV RC=%d", WEXITSTATUS(status));
            push_tail(st, boottime_ms(), "M", TailKind::ModuleError, buf, "MOD:LOGCAT_EV");
        }
        if (!st->stop) usleep(400000);
    }
    return nullptr;
}

static void append_kv_tokens(const std::string& raw, std::vector<std::pair<std::string, std::string>>& kvs) {
    size_t start = 0;
    while (start < raw.size()) {
        size_t comma = raw.find(',', start);
        std::string item = (comma == std::string::npos) ? raw.substr(start) : raw.substr(start, comma - start);
        size_t colon = item.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < item.size()) {
            kvs.emplace_back(item.substr(0, colon), item.substr(colon + 1));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

std::string build_bootloader_line() {
    std::vector<std::pair<std::string, std::string>> kvs;
    append_kv_tokens(get_prop("ro.boot.boottime"), kvs);
    append_kv_tokens(get_prop("ro.boot.boottime_extra"), kvs);
    if (kvs.empty()) return "BOOTLOADER TIMING N/A";

    const char* preferred[] = {"2BLE", "3BLE", "AVB", "KL", "DISP", "DRAM_TRAIN", "FWL"};
    std::string line;
    for (const char* key : preferred) {
        for (const auto& kv : kvs) {
            if (kv.first == key && kv.second != "0") {
                std::string tok = (line.empty() ? "" : " ") + kv.first + ":" + kv.second;
                if (line.size() + tok.size() <= 42) {
                    line += tok;
                }
                break;
            }
        }
    }
    if (line.empty()) {
        for (const auto& kv : kvs) {
            std::string tok = (line.empty() ? "" : " ") + kv.first + ":" + kv.second;
            if (line.size() + tok.size() <= 42) {
                line += tok;
            } else {
                break;
            }
        }
    }
    return line.empty() ? "BOOTLOADER TIMING N/A" : line;
}

std::string short_init_action(const std::string& act) {
    if (act == "load_persist_props_action") return "PERSIST_PROPS";
    if (act == "firmware_mounts_complete") return "FW_MOUNTS";
    return upper_copy(act);
}

void init_phases(EventState* st, const std::string& bl_detail) {
    std::lock_guard<std::mutex> lock(st->mu);
    const char* ids[] = {"BL", "INIT", "ZYG", "PRE", "PMS", "AMS"};
    for (int i = 0; i < 6; i++) {
        st->phases[i] = PhaseNode{};
        st->phases[i].id = ids[i];
    }
    st->phases[0].state = NodeState::Done;
    st->phases[0].start_ms = -1;
    st->phases[0].end_ms = -1;
    st->phases[0].detail = bl_detail;

    st->phases[1].state = NodeState::Running;
    st->phases[1].start_ms = 0;
    st->phases[1].detail = "EARLY-INIT -> ZYGOTE";

    st->phases[2].detail = "ZYGOTE -> SYSTEM_SERVER";
    st->phases[3].detail = "PRELOAD CLASSES";
    st->phases[4].detail = "SYSTEM / DATA PKG SCAN";
    st->phases[5].detail = "AMS_READY -> SCREEN";

    std::string zyg_ns_str = get_prop("ro.boottime.zygote");
    if (!zyg_ns_str.empty()) {
        long long zyg_ns = strtoll(zyg_ns_str.c_str(), nullptr, 10);
        if (zyg_ns > 0) {
            int64_t zyg_ms = (int64_t)(zyg_ns / 1000000LL);
            st->phases[1].state = NodeState::Done;
            st->phases[1].end_ms = zyg_ms;
            if (st->phases[2].state == NodeState::Pending) {
                st->phases[2].state = NodeState::Running;
                st->phases[2].start_ms = zyg_ms;
            }
        }
    }
}

std::string format_phase_row(const PhaseNode& n, int64_t now_ms, const RuleSet& rules) {
    const char* badge = "-PND";
    if (n.state == NodeState::Done) badge = "+OK ";
    else if (n.state == NodeState::Running) badge = ">RUN";

    char dur_buf[16];
    int64_t dur_ms = -1;
    if (n.state == NodeState::Done && n.start_ms >= 0 && n.end_ms >= n.start_ms) {
        dur_ms = n.end_ms - n.start_ms;
    } else if (n.state == NodeState::Running && n.start_ms >= 0 && now_ms >= n.start_ms) {
        dur_ms = now_ms - n.start_ms;
    }
    if (dur_ms >= 0) {
        double sec = dur_ms / 1000.0;
        if (sec < 100.0) snprintf(dur_buf, sizeof(dur_buf), "%5.2fS", sec);
        else if (sec < 1000.0) snprintf(dur_buf, sizeof(dur_buf), "%5.1fS", sec);
        else snprintf(dur_buf, sizeof(dur_buf), "%5.0fS", sec);
    } else if (strcmp(n.id, "BL") == 0) {
        snprintf(dur_buf, sizeof(dur_buf), "  RAW ");
    } else {
        snprintf(dur_buf, sizeof(dur_buf), "      ");
    }

    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s %s %-4s ", badge, dur_buf, n.id);
    std::string det = sanitize_tail_text(rules, n.detail, 41);
    std::string row = std::string(prefix) + det;
    if (row.size() > kTailLineChars) row.resize(kTailLineChars);
    return row;
}

std::string format_tail_row(const TailEntry& e) {
    char tbuf[16];
    if (e.ts_ms < 0) {
        snprintf(tbuf, sizeof(tbuf), " -.--");
    } else {
        double sec = e.ts_ms / 1000.0;
        if (sec < 100.0) {
            snprintf(tbuf, sizeof(tbuf), "%5.2f", sec);
        } else if (sec < 1000.0) {
            snprintf(tbuf, sizeof(tbuf), "%5.1f", sec);
        } else {
            snprintf(tbuf, sizeof(tbuf), "%5lld", (long long)llround(sec));
        }
    }
    std::string line = std::string(tbuf) + " " + e.source + " " + e.text;
    if (line.size() > kTailLineChars) line.resize(kTailLineChars);
    return line;
}

}  // namespace bootscope
