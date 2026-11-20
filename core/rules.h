#pragma once

#include <stddef.h>
#include <string>
#include <vector>

namespace bootscope {

struct RuleSet {
    std::vector<std::string> ignore_cmd;
    std::vector<std::string> ignore_svc;
    std::vector<std::string> ignore_tag;
    std::vector<std::string> module_key;
    std::vector<std::string> redact_key;
    std::string source = "BUILTIN";
    size_t count = 0;
};

void init_builtin_rules(RuleSet* r);
void load_rules(RuleSet* r);
bool has_sensitive_marker(const RuleSet& rules, const std::string& raw);
bool matches_module_key(const RuleSet& rules, const std::string& raw);
bool matches_ignore_cmd(const RuleSet& rules, const std::string& full_cmd);
bool matches_ignore_svc(const RuleSet& rules, const std::string& svc);
bool matches_ignore_tag(const RuleSet& rules, const std::string& tag);
std::string sanitize_tail_text(const RuleSet& rules, const std::string& raw, size_t max_len);

}  // namespace bootscope
