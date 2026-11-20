#pragma once

#include <string>

namespace bootscope {

enum class DisplayMode {
    Early,
    Preview,
    Replay,
};

struct DisplayOptions {
    DisplayMode mode = DisplayMode::Early;
    std::string replay_dir;
    bool replay_bl = false;
    bool replay_pre_sf = false;
    float replay_speed = 1.0f;
    bool in_tmp = false;
};

int run_display(const DisplayOptions& opts, int argc, char** argv);

}  // namespace bootscope
