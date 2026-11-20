#pragma once

#include <string>

namespace bootscope {

struct CollectOptions {
    int keep_history = -1;
    bool once = false;
    std::string out_dir;
};

int prune_old_runs(
    int keep, const std::string& protect_boot_id, int* kept_out, int* deleted_out);

int run_collector(const CollectOptions& opts);

}  // namespace bootscope
