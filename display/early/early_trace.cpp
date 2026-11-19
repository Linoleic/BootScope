#include "early/early_trace.h"
#include "early/common.h"

#include <stdio.h>
#include <mutex>

namespace bootscope {

static std::mutex g_trace_mu;
static FILE* g_trace_fp = nullptr;
static size_t g_trace_bytes = 0;
constexpr size_t kMaxTraceBytes = 512 * 1024;

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

void trace_tail(int64_t ts_ms, const char* source, const char* kind, const std::string& text) {
    if (!trace_is_open()) return;
    std::string line = std::to_string(ts_ms) + "\tTAIL\t" + source + "\t" + kind
        + "\t" + clean_tsv_field(text);
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
