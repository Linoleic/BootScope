#pragma once

#include <string>

namespace bootscope {

int cmd_list_json(int limit);
int cmd_inspect_json(const std::string& target);
int cmd_compare_json(const std::string& target_a, const std::string& target_b);
int cmd_summary(const std::string& target);
int cmd_prune_json(int keep);
int cmd_config_get_json();
int cmd_config_set_json(const std::string& key, const std::string& val);
int cmd_rules_get_json();
int cmd_rules_set_json();

}  // namespace bootscope
