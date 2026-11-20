#include "early_trace.h"
#include "common.h"

#include <stdio.h>
#include <string.h>
#include <mutex>
#include <unordered_map>

namespace bootscope {

static std::mutex g_trace_mu;
static FILE* g_trace_fp = nullptr;
static size_t g_trace_bytes = 0;
static std::unordered_map<std::string, std::string> g_last_kv;
static std::string g_last_bl_sig;
constexpr size_t kMaxTraceBytes = 2 * 1024 * 1024;

static std::string clean_tsv_field(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\t' || c == '\r' || c == '\n') out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

static void write_trace_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_trace_mu);
    if (!g_trace_fp || g_trace_bytes + line.size() + 1 > kMaxTraceBytes) return;
    size_t n = fwrite(line.data(), 1, line.size(), g_trace_fp);
    fputc('\n', g_trace_fp);
    fflush(g_trace_fp);
    g_trace_bytes += n + 1;
}

void trace_open() {
    std::lock_guard<std::mutex> lock(g_trace_mu);
    if (g_trace_fp) return;
    g_trace_fp = fopen(kEarlyTracePath, "w");
    g_trace_bytes = 0;
    g_last_kv.clear();
    g_last_bl_sig.clear();
    if (g_trace_fp) {
        setvbuf(g_trace_fp, nullptr, _IOLBF, 0);
        const char* hdr = "# ts_ms\ttype\tfields...\n";
        fwrite(hdr, 1, strlen(hdr), g_trace_fp);
        g_trace_bytes = strlen(hdr);
    }
}

void trace_close() {
    std::lock_guard<std::mutex> lock(g_trace_mu);
    if (g_trace_fp) {
        fflush(g_trace_fp);
        fclose(g_trace_fp);
        g_trace_fp = nullptr;
    }
}

bool trace_is_open() {
    std::lock_guard<std::mutex> lock(g_trace_mu);
    return g_trace_fp != nullptr;
}

void trace_life(int64_t ts_ms, const char* event, const std::string& detail) {
    if (!trace_is_open()) return;
    std::string line = std::to_string(ts_ms) + "\tLIFE\t" + event;
    if (!detail.empty()) {
        line += "\t" + clean_tsv_field(detail);
    }
    write_trace_line(line);
}

void trace_bl(int64_t ts_ms, const std::string& bl_line, int64_t total_ms) {
    if (!trace_is_open()) return;
    std::string line = std::to_string(ts_ms) + "\tBL\t" + std::to_string(total_ms)
        + "\t" + clean_tsv_field(bl_line);
    write_trace_line(line);
}

void trace_bl_meta(
        const std::string& source, int64_t timeline_ms, int64_t wait_ms, const std::string& meta_line) {
    if (!trace_is_open()) return;
    std::string line = "0\tBLMETA\t" + clean_tsv_field(source)
        + "\t" + std::to_string(timeline_ms)
        + "\t" + std::to_string(wait_ms)
        + "\t" + clean_tsv_field(meta_line);
    write_trace_line(line);
}

void trace_bl_stage(const BlStage& st) {
    if (!trace_is_open()) return;
    std::string line = "0\tBLSTG\t" + clean_tsv_field(st.id)
        + "\t" + std::to_string(st.start_ms)
        + "\t" + std::to_string(st.dur_ms)
        + "\t" + (st.wait_only ? "1" : "0")
        + "\t" + clean_tsv_field(st.detail);
    write_trace_line(line);
}

void trace_bl_event(const BlEvent& ev) {
    if (!trace_is_open()) return;
    std::string line = "0\tBLEV\t" + std::to_string(ev.por_ms)
        + "\t" + (ev.major ? "1" : "0")
        + "\t" + clean_tsv_field(ev.text);
    write_trace_line(line);
}

void trace_bl_profile(int64_t ts_ms, const BootloaderProfile& profile) {
    if (!trace_is_open()) return;
    std::string sig = profile.source + "|" + std::to_string(profile.total_ms) + "|"
        + std::to_string(profile.stages.size()) + "|" + profile.summary_line + "|" + profile.meta_line;
    {
        std::lock_guard<std::mutex> lock(g_trace_mu);
        if (g_last_bl_sig == sig) return;
        g_last_bl_sig = sig;
    }
    trace_bl(ts_ms, profile.summary_line, profile.total_ms);
    if (!profile.source.empty() || !profile.stages.empty()) {
        trace_bl_meta(profile.source, profile.timeline_ms, profile.wait_ms, profile.meta_line);
        for (const auto& st : profile.stages) {
            trace_bl_stage(st);
        }
        for (const auto& ev : profile.events) {
            trace_bl_event(ev);
        }
    }
}

void trace_phase(
        int64_t ts_ms, int idx, int state, int64_t start_ms, int64_t end_ms,
        const std::string& detail) {
    if (!trace_is_open()) return;
    char buf[96];
    snprintf(buf, sizeof(buf), "%lld\tPHASE\t%d\t%d\t%lld\t%lld",
             (long long)ts_ms, idx, state, (long long)start_ms, (long long)end_ms);
    std::string line = std::string(buf) + "\t" + clean_tsv_field(detail);
    write_trace_line(line);
}

void trace_stage(int64_t ts_ms, int step, const std::string& label, const std::string& raw_ms) {
    if (!trace_is_open()) return;
    std::string line = std::to_string(ts_ms) + "\tSTAGE\t" + std::to_string(step)
        + "\t" + clean_tsv_field(label) + "\t" + clean_tsv_field(raw_ms);
    write_trace_line(line);
}

void trace_tail(int64_t ts_ms, const char* source, const char* kind, const std::string& text) {
    if (!trace_is_open()) return;
    std::string line = std::to_string(ts_ms) + "\tTAIL\t" + source + "\t" + kind
        + "\t" + clean_tsv_field(text);
    write_trace_line(line);
}

void trace_kv(int64_t ts_ms, const std::string& key, const std::string& text, bool alert) {
    if (!trace_is_open() || key.empty()) return;
    std::string val_token = std::string(alert ? "1\t" : "0\t") + clean_tsv_field(text);
    {
        std::lock_guard<std::mutex> lock(g_trace_mu);
        auto it = g_last_kv.find(key);
        if (it != g_last_kv.end() && it->second == val_token) return;
        g_last_kv[key] = val_token;
    }
    std::string line = std::to_string(ts_ms) + "\tKV\t" + clean_tsv_field(key)
        + "\t" + clean_tsv_field(text) + "\t" + (alert ? "1" : "0");
    write_trace_line(line);
}

void trace_metric(
        int64_t ts_ms, int cpu_pct, int io_pct, int mem_pct,
        const SysMetrics& sys, const std::string* cur_states, int svc_count) {
    if (!trace_is_open()) return;
    std::string top_str;
    for (size_t i = 0; i < sys.top_cpu.size() && i < 3; i++) {
        if (i > 0) top_str += ",";
        char tb[48];
        snprintf(tb, sizeof(tb), "%s:%.1f", clean_tsv_field(sys.top_cpu[i].comm).c_str(), sys.top_cpu[i].pct);
        top_str += tb;
    }
    if (top_str.empty()) top_str = "-";

    std::string svc_str;
    for (int i = 0; i < svc_count; i++) {
        if (i > 0) svc_str += ",";
        svc_str += cur_states[i];
    }

    char head[96];
    snprintf(head, sizeof(head), "%lld\tMETRIC\t%d,%d,%d",
             (long long)ts_ms, cpu_pct, io_pct, mem_pct);
    std::string line = std::string(head)
        + "\t" + clean_tsv_field(sys.freq_line)
        + "\t" + clean_tsv_field(sys.mem_line)
        + "\t" + clean_tsv_field(sys.io_line)
        + "\t" + top_str
        + "\t" + svc_str;
    write_trace_line(line);
}

}  // namespace bootscope
