#include "rules.h"
#include "canvas.h"
#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>

namespace bootscope {

void init_builtin_rules(RuleSet* r) {
    r->ignore_cmd.clear();
    r->ignore_svc.clear();
    r->ignore_tag.clear();
    r->module_key = {"bootscope"};
    r->redact_key = {
        "imei", "imsi", "iccid", "bssid", "ssid=", "password",
        "secret", "token=", "cookie", "serialno=", "android_id",
        "mac=", "macaddr",
    };
    r->source = "BUILTIN";
    r->count = r->module_key.size() + r->redact_key.size();
}

void load_rules(RuleSet* r) {
    init_builtin_rules(r);
    struct Candidate {
        const char* path;
        const char* label;
    };
    static const Candidate kPaths[] = {
        {kUserRulesPath, "USR"},
        {kEarlyRulesPath, "DEF"},
        {kModuleRulesPath, "DEF"},
    };
    FILE* f = nullptr;
    const char* chosen_label = "BUILTIN";
    for (const auto& c : kPaths) {
        if (access(c.path, R_OK) == 0) {
            f = fopen(c.path, "r");
            if (f) {
                chosen_label = c.label;
                break;
            }
        }
    }
    if (!f) return;

    RuleSet parsed;
    parsed.source = chosen_label;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim_copy(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= s.size()) continue;
        std::string key = lower_copy(trim_copy(s.substr(0, eq)));
        std::string val = lower_copy(trim_copy(s.substr(eq + 1)));
        if (val.empty()) continue;
        if (key == "ignore_cmd") parsed.ignore_cmd.push_back(val);
        else if (key == "ignore_svc") parsed.ignore_svc.push_back(val);
        else if (key == "ignore_tag") parsed.ignore_tag.push_back(val);
        else if (key == "module_key") parsed.module_key.push_back(val);
        else if (key == "redact_key") parsed.redact_key.push_back(val);
    }
    fclose(f);

    if (parsed.module_key.empty()) parsed.module_key = r->module_key;
    if (parsed.redact_key.empty()) parsed.redact_key = r->redact_key;
    parsed.count = parsed.ignore_cmd.size() + parsed.ignore_svc.size()
        + parsed.ignore_tag.size() + parsed.module_key.size() + parsed.redact_key.size();
    *r = std::move(parsed);
}

bool has_sensitive_marker(const RuleSet& rules, const std::string& raw) {
    for (const auto& kw : rules.redact_key) {
        if (icontains(raw, kw.c_str())) return true;
    }
    return false;
}

bool matches_module_key(const RuleSet& rules, const std::string& raw) {
    for (const auto& kw : rules.module_key) {
        if (icontains(raw, kw.c_str())) return true;
    }
    return false;
}

bool matches_ignore_cmd(const RuleSet& rules, const std::string& full_cmd) {
    std::string low = lower_copy(full_cmd);
    for (const auto& prefix : rules.ignore_cmd) {
        if (low == prefix) return true;
        if (low.size() > prefix.size() && low.compare(0, prefix.size(), prefix) == 0) {
            char back = prefix.back();
            if (back == '-' || back == '_' || back == '/' || low[prefix.size()] == ' ') {
                return true;
            }
        }
    }
    return false;
}

bool matches_ignore_svc(const RuleSet& rules, const std::string& svc) {
    for (const auto& rule : rules.ignore_svc) {
        if (iequals(svc, rule)) return true;
    }
    return false;
}

bool matches_ignore_tag(const RuleSet& rules, const std::string& tag) {
    for (const auto& rule : rules.ignore_tag) {
        if (iequals(tag, rule)) return true;
    }
    return false;
}

std::string sanitize_tail_text(const RuleSet& rules, const std::string& raw, size_t max_len) {
    if (raw.empty() || has_sensitive_marker(rules, raw)) return "";
    std::string out;
    out.reserve(std::min(raw.size(), max_len));
    bool prev_space = true;
    for (unsigned char uc : raw) {
        char ch = (char)toupper(uc);
        bool keep = (strchr(kFontChars, ch) != nullptr);
        bool map_space = (ch == ' ' || ch == '\t' || ch == '(' || ch == ')'
                          || ch == '[' || ch == ']' || ch == ',' || ch == ';'
                          || ch == '\'' || ch == '"' || ch == '!' || ch == '|');
        if (keep) {
            out.push_back(ch);
            prev_space = false;
        } else if (map_space) {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
        }
        if (out.size() >= max_len) break;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace bootscope
