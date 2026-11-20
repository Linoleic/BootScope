#include "collector.h"
#include "json_api.h"
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace {

void print_usage() {
    fprintf(stderr,
            "Usage: bootscope <subcommand> [options]\n"
            "  early [--in-tmp]\n"
            "  preview\n"
            "  replay <latest|boot_id|dir> [--bl] [--pre-sf] [--speed <0.25..10>]\n"
            "  collect [--keep <N>] [--once] [--out <dir>]\n"
            "  list-json [--limit <N>]\n"
            "  inspect-json <latest|boot_id|dir>\n"
            "  logs-json <latest|boot_id|dir> [--level <all|critical|merr|fatal|init_fail|error|stage|info>] [--tag <tag>] [--q <query>] [--limit <N>] [--order <group|time>]\n"
            "  compare-json <target_a> <target_b>\n"
            "  summary <latest|boot_id|dir>\n"
            "  prune --keep <N>\n"
            "  config-get | config-set <key> <val>\n"
            "  rules-get  | rules-set\n");
}

}  // namespace

int main(int argc, char** argv) {
    using namespace bootscope;

    std::string sub = (argc >= 2) ? argv[1] : "early";
    int arg_start = 2;

    if (sub == "--preview" || sub == "--replay" || sub == "--in-tmp"
            || sub == "--bl" || sub == "--pre-sf" || sub == "--speed") {
        sub = "early";
        arg_start = 1;
    }

    if (sub == "early" || sub == "preview" || sub == "replay") {
        DisplayOptions opts;
        if (sub == "preview") opts.mode = DisplayMode::Preview;
        else if (sub == "replay") opts.mode = DisplayMode::Replay;
        else opts.mode = DisplayMode::Early;

        for (int i = arg_start; i < argc; i++) {
            const char* a = argv[i];
            if (strcmp(a, "--preview") == 0) {
                opts.mode = DisplayMode::Preview;
            } else if (strcmp(a, "--replay") == 0 && i + 1 < argc) {
                opts.mode = DisplayMode::Replay;
                opts.replay_dir = argv[++i];
            } else if (strcmp(a, "--bl") == 0) {
                opts.replay_bl = true;
            } else if (strcmp(a, "--pre-sf") == 0) {
                opts.replay_pre_sf = true;
            } else if (strcmp(a, "--speed") == 0 && i + 1 < argc) {
                float sp = (float)atof(argv[++i]);
                if (sp >= 0.25f && sp <= 10.0f) opts.replay_speed = sp;
            } else if (strcmp(a, "--in-tmp") == 0) {
                opts.in_tmp = true;
            } else if (opts.mode == DisplayMode::Replay && opts.replay_dir.empty() && a[0] != '-') {
                opts.replay_dir = a;
            }
        }
        return run_display(opts, argc, argv);
    }

    if (sub == "collect") {
        CollectOptions opts;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--once") == 0) {
                opts.once = true;
            } else if (strcmp(argv[i], "--keep") == 0 && i + 1 < argc) {
                opts.keep_history = atoi(argv[++i]);
            } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
                opts.out_dir = argv[++i];
            }
        }
        return run_collector(opts);
    }

    if (sub == "list-json") {
        int limit = 30;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
                limit = atoi(argv[++i]);
            }
        }
        return cmd_list_json(limit);
    }

    if (sub == "inspect-json") {
        std::string target = (argc >= 3) ? argv[2] : "latest";
        return cmd_inspect_json(target);
    }

    if (sub == "logs-json") {
        std::string target = "latest";
        std::string level = "all";
        std::string tag;
        std::string query;
        std::string order = "group";
        int limit = 200;
        bool has_target = false;
        for (int i = 2; i < argc; i++) {
            const char* a = argv[i];
            if (strcmp(a, "--level") == 0 && i + 1 < argc) {
                level = argv[++i];
            } else if (strcmp(a, "--tag") == 0 && i + 1 < argc) {
                tag = argv[++i];
            } else if (strcmp(a, "--q") == 0 && i + 1 < argc) {
                query = argv[++i];
            } else if (strcmp(a, "--limit") == 0 && i + 1 < argc) {
                limit = atoi(argv[++i]);
            } else if (strcmp(a, "--order") == 0 && i + 1 < argc) {
                order = argv[++i];
            } else if (a[0] != '-' && !has_target) {
                target = a;
                has_target = true;
            }
        }
        return cmd_logs_json(target, level, tag, query, limit, order);
    }

    if (sub == "compare-json") {
        if (argc < 4) {
            printf("{\"ok\":false,\"error\":\"missing_targets\"}\n");
            return 1;
        }
        return cmd_compare_json(argv[2], argv[3]);
    }

    if (sub == "summary") {
        std::string target = (argc >= 3) ? argv[2] : "latest";
        return cmd_summary(target);
    }

    if (sub == "prune") {
        int keep = 10;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--keep") == 0 && i + 1 < argc) {
                keep = atoi(argv[++i]);
            } else if (argv[i][0] != '-') {
                keep = atoi(argv[i]);
            }
        }
        return cmd_prune_json(keep);
    }

    if (sub == "config-get") {
        return cmd_config_get_json();
    }

    if (sub == "config-set") {
        if (argc < 4) {
            printf("{\"ok\":false,\"error\":\"missing_key_val\"}\n");
            return 1;
        }
        return cmd_config_set_json(argv[2], argv[3]);
    }

    if (sub == "rules-get") {
        return cmd_rules_get_json();
    }

    if (sub == "rules-set") {
        return cmd_rules_set_json();
    }

    print_usage();
    return 1;
}
