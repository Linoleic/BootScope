#include "early/bootloader.h"
#include "early/common.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace bootscope {
namespace {

bool read_bounded_text(const std::string& path, size_t max_bytes, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    out->resize(max_bytes);
    size_t n = fread(&(*out)[0], 1, max_bytes, f);
    fclose(f);
    out->resize(n);
    for (char& ch : *out) {
        if (ch == '\0') ch = '\n';
    }
    return n > 0;
}

std::vector<std::string> split_lines_trim(const std::string& raw) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        std::string line = (nl == std::string::npos) ? raw.substr(pos) : raw.substr(pos, nl - pos);
        std::string trimmed = trim_copy(line);
        if (!trimmed.empty()) lines.push_back(std::move(trimmed));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

void parse_kv_list(
        const std::string& raw,
        std::vector<std::pair<std::string, int64_t>>* ordered,
        std::unordered_map<std::string, int64_t>* map_out) {
    size_t start = 0;
    while (start < raw.size()) {
        size_t comma = raw.find(',', start);
        std::string item = (comma == std::string::npos) ? raw.substr(start) : raw.substr(start, comma - start);
        item = trim_copy(item);
        size_t colon = item.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < item.size()) {
            std::string key = upper_copy(trim_copy(item.substr(0, colon)));
            std::string val_str = trim_copy(item.substr(colon + 1));
            char* endp = nullptr;
            long long val = strtoll(val_str.c_str(), &endp, 10);
            if (endp != val_str.c_str() && *endp == '\0' && val >= 0) {
                ordered->emplace_back(key, (int64_t)val);
                (*map_out)[key] = (int64_t)val;
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

int64_t lookup_val(const std::unordered_map<std::string, int64_t>& m, const char* key) {
    auto it = m.find(key);
    return (it != m.end()) ? it->second : 0;
}

bool extract_last_int_after(const std::string& line, const char* marker, int64_t* out) {
    size_t pos = line.find(marker);
    if (pos == std::string::npos) return false;
    pos += strlen(marker);
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == ':' || line[pos] == '=' || line[pos] == '[')) {
        pos++;
    }
    if (pos >= line.size() || !isdigit((unsigned char)line[pos])) return false;
    char* endp = nullptr;
    long long val = strtoll(line.c_str() + pos, &endp, 10);
    if (endp == line.c_str() + pos || val < 0) return false;
    *out = (int64_t)val;
    return true;
}

void sort_bl_events(std::vector<BlEvent>* events) {
    std::stable_sort(events->begin(), events->end(), [](const BlEvent& a, const BlEvent& b) {
        return a.por_ms < b.por_ms;
    });
}

void append_summary_tok(std::string* line, const std::string& tok, size_t max_len = 41) {
    std::string piece = line->empty() ? tok : (" " + tok);
    if (line->size() + piece.size() <= max_len) {
        *line += piece;
    }
}

void append_meta_tok(std::string* meta, const std::string& tok, size_t max_len = 46) {
    if (tok.empty()) return;
    std::string piece = meta->empty() ? tok : (" " + tok);
    if (meta->size() + piece.size() <= max_len) {
        *meta += piece;
    }
}

std::string format_sw_compact(int64_t ms) {
    char b[24];
    if (ms >= 1000) {
        snprintf(b, sizeof(b), "SW:%.1fS", ms / 1000.0);
    } else {
        snprintf(b, sizeof(b), "SW:%lld", (long long)ms);
    }
    return b;
}

}  // namespace

bool parse_boottime_props(
        const std::string& boottime, const std::string& boottime_extra, BootloaderProfile* out) {
    *out = BootloaderProfile{};
    std::vector<std::pair<std::string, int64_t>> ordered;
    std::unordered_map<std::string, int64_t> kv;
    parse_kv_list(boottime, &ordered, &kv);
    std::vector<std::pair<std::string, int64_t>> extra_ordered;
    std::unordered_map<std::string, int64_t> extra_kv;
    parse_kv_list(boottime_extra, &extra_ordered, &extra_kv);

    if (ordered.empty() && extra_ordered.empty()) return false;

    out->source = "PROP";
    int64_t sw_ms = lookup_val(kv, "SW");
    out->wait_ms = sw_ms;

    bool has_pixel_stages = kv.count("0BLE") || kv.count("1BLE") || kv.count("2BLE") || kv.count("3BLE");
    int64_t cursor_ms = 0;

    if (has_pixel_stages) {
        int64_t bl0 = lookup_val(kv, "0BLE");
        int64_t bl1 = lookup_val(kv, "1BLL") + lookup_val(kv, "1BLE");
        int64_t bl2 = lookup_val(kv, "2BLL") + lookup_val(kv, "2BLE");
        int64_t bl3 = lookup_val(kv, "3BLL") + lookup_val(kv, "3BLE");
        int64_t avb = lookup_val(kv, "AVB");
        int64_t kl = lookup_val(kv, "KL") + lookup_val(kv, "KD");
        int64_t odt = lookup_val(kv, "ODT");
        int64_t fwl = lookup_val(kv, "FWL");

        int64_t ddr_train = lookup_val(extra_kv, "DRAM_TRAIN");
        int64_t disp = lookup_val(extra_kv, "DISP");
        int64_t kl_boot = lookup_val(extra_kv, "KL_BOOT");
        int64_t kl_vdrb = lookup_val(extra_kv, "KL_VDRB");
        int64_t kl_dtbo = lookup_val(extra_kv, "KL_DTBO");
        int64_t pkd = lookup_val(extra_kv, "PKD");

        auto push_stage = [&](const char* id, int64_t dur, const std::string& detail, bool major) {
            if (dur <= 0) return;
            BlStage st;
            st.id = id;
            st.start_ms = cursor_ms;
            st.dur_ms = dur;
            st.detail = detail;
            out->stages.push_back(st);
            char eb[64];
            snprintf(eb, sizeof(eb), "%s %lldMS", id, (long long)dur);
            out->events.push_back({cursor_ms, major, eb});
            cursor_ms += dur;
        };

        push_stage("BL0", bl0, "BOOT ROM INIT", true);
        if (bl1 > 0) {
            char d[48];
            snprintf(d, sizeof(d), "LOAD:%lldMS EXEC:%lldMS",
                     (long long)lookup_val(kv, "1BLL"), (long long)lookup_val(kv, "1BLE"));
            push_stage("BL1", bl1, d, false);
        }
        if (bl2 > 0) {
            std::string d = "LOAD:" + std::to_string(lookup_val(kv, "2BLL"))
                + " EXEC:" + std::to_string(lookup_val(kv, "2BLE"));
            if (ddr_train > 0) {
                d += " DDR:" + std::to_string(ddr_train) + "MS";
                out->events.push_back({cursor_ms + std::min(bl2 / 2, ddr_train), false,
                                       "DRAM_TRAIN " + std::to_string(ddr_train) + "MS"});
            }
            push_stage("BL2", bl2, d, true);
        }
        if (bl3 > 0) {
            std::string d = "LOAD:" + std::to_string(lookup_val(kv, "3BLL"))
                + " EXEC:" + std::to_string(lookup_val(kv, "3BLE"));
            if (disp > 0) {
                d += " DISP:" + std::to_string(disp) + "MS";
                out->events.push_back({cursor_ms + std::min(bl3 / 2, disp), false,
                                       "DISPLAY INIT " + std::to_string(disp) + "MS"});
            }
            push_stage("BL3", bl3, d, true);
        }
        push_stage("AVB", avb, "VERIFIED BOOT", true);
        if (kl > 0) {
            std::string d = "KERNEL LOAD";
            if (kl_boot > 0 || kl_vdrb > 0 || kl_dtbo > 0) {
                d = "BOOT:" + std::to_string(kl_boot)
                    + " VDRB:" + std::to_string(kl_vdrb)
                    + " DTBO:" + std::to_string(kl_dtbo);
            }
            if (kl_boot > 0) {
                out->events.push_back({cursor_ms, false, "KL_BOOT " + std::to_string(kl_boot) + "MS"});
            }
            if (kl_vdrb > 0) {
                out->events.push_back({cursor_ms + std::max<int64_t>(1, kl / 2), false,
                                       "KL_VDRB " + std::to_string(kl_vdrb) + "MS"});
            }
            push_stage("KL", kl, d, true);
        } else if (kl_boot > 0 || kl_vdrb > 0) {
            out->events.push_back({cursor_ms, false,
                                   "KL EXTRA BOOT:" + std::to_string(kl_boot) + " VDRB:" + std::to_string(kl_vdrb)});
        }
        if (odt > 0) {
            push_stage("ODT", odt, "DEVICE TREE OVERLAY", false);
        }
        if (fwl > 0) {
            std::string d = "FIRMWARE LOAD";
            if (pkd > 0) d += " PKD:" + std::to_string(pkd) + "MS";
            push_stage("FWL", fwl, d, true);
        }
        out->events.push_back({cursor_ms, true, "KERNEL HANDOFF"});
        if (sw_ms > 0) {
            out->stages.push_back({"SW", "UNLOCK WARNING WAIT", cursor_ms, sw_ms, true});
            out->events.push_back({cursor_ms, false, "SW WAIT " + std::to_string(sw_ms) + "MS"});
        }

        out->total_ms = cursor_ms;
        out->timeline_ms = cursor_ms;
        out->summary_line.clear();
        if (bl2 > 0) append_summary_tok(&out->summary_line, "BL2:" + std::to_string(bl2));
        if (bl3 > 0) append_summary_tok(&out->summary_line, "BL3:" + std::to_string(bl3));
        if (avb > 0) append_summary_tok(&out->summary_line, "AVB:" + std::to_string(avb));
        if (fwl > 0) append_summary_tok(&out->summary_line, "FWL:" + std::to_string(fwl));
        if (kl > 0) append_summary_tok(&out->summary_line, "KL:" + std::to_string(kl));
        if (sw_ms > 0) append_summary_tok(&out->summary_line, format_sw_compact(sw_ms));
    } else {
        out->summary_line.clear();
        for (const auto& kvp : ordered) {
            if (kvp.second <= 0) continue;
            if (kvp.first == "SW") {
                out->stages.push_back({"SW", "WARNING WAIT", cursor_ms, kvp.second, true});
                continue;
            }
            out->stages.push_back({kvp.first, kvp.first + " " + std::to_string(kvp.second) + "MS",
                                   cursor_ms, kvp.second, false});
            out->events.push_back({cursor_ms, true, kvp.first + " " + std::to_string(kvp.second) + "MS"});
            cursor_ms += kvp.second;
            append_summary_tok(&out->summary_line, kvp.first + ":" + std::to_string(kvp.second));
        }
        out->total_ms = cursor_ms;
        out->timeline_ms = cursor_ms;
        if (sw_ms > 0) append_summary_tok(&out->summary_line, format_sw_compact(sw_ms));
    }

    if (out->summary_line.empty()) out->summary_line = "BOOTLOADER TIMING N/A";
    sort_bl_events(&out->events);
    return !out->stages.empty() || out->total_ms > 0;
}

bool parse_qcom_uefilog_file(const std::string& path, BootloaderProfile* out) {
    *out = BootloaderProfile{};
    std::string raw;
    if (!read_bounded_text(path, 131072, &raw)) return false;

    int64_t apbl_media_start_us = -1, apbl_media_end_us = -1;
    int64_t apbl_exit_us = -1;
    int64_t sbl1_start_us = -1, sbl1_end_us = -1, sbl1_delta_us = -1;
    int64_t ddr_init_us = -1, ddr_done_us = -1;
    int64_t ddr_freq_mhz = -1;
    int64_t uefi_start_ms = -1, uefi_total_ms = -1, logo_ms = -1;
    int64_t boot_img_ms = -1, vboot_ms = -1, dtbo_ms = -1, init_boot_ms = -1, vbmeta_ms = -1;
    int64_t overlay_ms = -1, dt_ms = -1, ebs_ms = -1;
    std::string reset_reason;
    std::string cold_boot;
    std::string boot_dev;
    std::string slot;

    for (const std::string& line : split_lines_trim(raw)) {
        if (line.find("APBL Boot Media Detect Start:") == 0) {
            extract_last_int_after(line, "Start:", &apbl_media_start_us);
        } else if (line.find("APBL Boot Media Detect End:") == 0) {
            extract_last_int_after(line, "End:", &apbl_media_end_us);
        } else if (line.find("APBL Exit:") == 0) {
            extract_last_int_after(line, "APBL Exit:", &apbl_exit_us);
        } else if (line.rfind("B -", 0) == 0) {
            int64_t us = -1;
            if (sscanf(line.c_str(), "B - %lld -", (long long*)&us) == 1 && us >= 0) {
                if (line.find("SBL1, Start") != std::string::npos) sbl1_start_us = us;
                else if (line.find("SBL1, End") != std::string::npos) sbl1_end_us = us;
                else if (line.find("DDR_INIT") != std::string::npos && ddr_init_us < 0) ddr_init_us = us;
                else if (line.find("End of DDR Training Restore") != std::string::npos) ddr_done_us = us;
                else if (line.find("Max enabled DDR Freq =") != std::string::npos) {
                    extract_last_int_after(line, "=", &ddr_freq_mhz);
                } else {
                    size_t rp = line.find("PM: Reset by ");
                    if (rp != std::string::npos) reset_reason = upper_copy(trim_copy(line.substr(rp + 13)));
                }
            }
        } else if (line.rfind("D -", 0) == 0 && line.find("SBL1, Delta") != std::string::npos) {
            sscanf(line.c_str(), "D - %lld -", (long long*)&sbl1_delta_us);
        } else if (line.rfind("S - DDR Frequency,", 0) == 0 && ddr_freq_mhz < 0) {
            extract_last_int_after(line, "Frequency,", &ddr_freq_mhz);
        } else if (line.rfind("S - Boot Interface:", 0) == 0 && boot_dev.empty()) {
            boot_dev = upper_copy(trim_copy(line.substr(19)));
        } else if (line.find("UEFI Start") != std::string::npos) {
            extract_last_int_after(line, "UEFI Start", &uefi_start_ms);
        } else if (line.find("UEFI Total :") != std::string::npos) {
            extract_last_int_after(line, "UEFI Total :", &uefi_total_ms);
        } else if (line.find("DeCompressLogoData:") != std::string::npos) {
            extract_last_int_after(line, "use time:", &logo_ms);
        } else if (line.find("Boot Device") != std::string::npos && boot_dev.empty()) {
            size_t col = line.find(':');
            if (col != std::string::npos) boot_dev = upper_copy(trim_copy(line.substr(col + 1)));
        } else if (line.find("IsColdBoot =") != std::string::npos && cold_boot.empty()) {
            int64_t cb = 0;
            if (extract_last_int_after(line, "IsColdBoot =", &cb)) cold_boot = cb ? "COLD" : "WARM";
        } else if (line.find("Booting from slot") != std::string::npos) {
            size_t lp = line.find('(');
            size_t rp = line.rfind(')');
            if (lp != std::string::npos && rp != std::string::npos && rp > lp + 1) {
                slot = upper_copy(line.substr(lp + 1, rp - lp - 1));
            }
        } else if (line.find("Load Image vbmeta_system_") == 0) {
            // Skip vbmeta_system when tracking vbmeta_*
        } else if (line.find("Load Image vbmeta_") == 0) {
            extract_last_int_after(line, "total time:", &vbmeta_ms);
        } else if (line.find("Load Image boot_") == 0) {
            extract_last_int_after(line, "total time:", &boot_img_ms);
        } else if (line.find("Load Image vendor_boot_") == 0) {
            extract_last_int_after(line, "total time:", &vboot_ms);
        } else if (line.find("Load Image dtbo_") == 0) {
            extract_last_int_after(line, "total time:", &dtbo_ms);
        } else if (line.find("Load Image init_boot_") == 0) {
            extract_last_int_after(line, "total time:", &init_boot_ms);
        } else if (line.find("Apply Overlay total time:") == 0) {
            extract_last_int_after(line, "total time:", &overlay_ms);
        } else if (line.find("Update Device Tree total time:") == 0) {
            extract_last_int_after(line, "total time:", &dt_ms);
        } else if (line.find("Shutting Down UEFI Boot Services:") == 0) {
            extract_last_int_after(line, "Boot Services:", &ebs_ms);
        } else if (line.find("Start EBS") == 0 && ebs_ms < 0) {
            extract_last_int_after(line, "Start EBS", &ebs_ms);
        }
    }

    if (ebs_ms <= 0 && sbl1_delta_us <= 0 && uefi_total_ms <= 0) return false;

    out->source = "UEFILOG";
    if (apbl_exit_us > 0) {
        int64_t pbl_ms = (int64_t)llround(apbl_exit_us / 1000.0);
        std::string pdet = "APBL MEDIA+AUTH";
        if (apbl_media_start_us >= 0 && apbl_media_end_us > apbl_media_start_us) {
            int64_t md = (int64_t)llround((apbl_media_end_us - apbl_media_start_us) / 1000.0);
            pdet = "MEDIA:" + std::to_string(md) + "MS AUTH+EXIT";
        }
        out->stages.push_back({"PBL", pdet, 0, pbl_ms, false});
        out->events.push_back({0, true, "APBL START"});
        out->events.push_back({pbl_ms, false, "APBL EXIT " + std::to_string(pbl_ms) + "MS"});
    }

    if (sbl1_start_us >= 0 && (sbl1_end_us > sbl1_start_us || sbl1_delta_us > 0)) {
        int64_t s_start = (int64_t)llround(sbl1_start_us / 1000.0);
        int64_t s_dur = (sbl1_delta_us > 0)
            ? (int64_t)llround(sbl1_delta_us / 1000.0)
            : (int64_t)llround((sbl1_end_us - sbl1_start_us) / 1000.0);
        std::string sdet = "SBL1/XBL";
        if (ddr_freq_mhz > 0) sdet += " DDR:" + std::to_string(ddr_freq_mhz) + "MHZ";
        out->stages.push_back({"SBL1", sdet, s_start, s_dur, false});
        out->events.push_back({s_start, true, "SBL1 START"});
        if (ddr_init_us > 0) {
            int64_t d_s = (int64_t)llround(ddr_init_us / 1000.0);
            if (ddr_done_us > ddr_init_us) {
                int64_t d_dur = (int64_t)llround((ddr_done_us - ddr_init_us) / 1000.0);
                out->events.push_back({d_s, false, "DDR INIT+TRAIN " + std::to_string(d_dur) + "MS"});
            } else {
                out->events.push_back({d_s, false, "DDR_INIT"});
            }
        }
        int64_t s_end = (sbl1_end_us > 0) ? (int64_t)llround(sbl1_end_us / 1000.0) : (s_start + s_dur);
        out->events.push_back({s_end, false, "SBL1 END " + std::to_string(s_dur) + "MS"});
    }

    if (uefi_start_ms >= 0 && uefi_total_ms > 0) {
        std::string udet = "UEFI DXE INIT";
        if (logo_ms >= 0) udet = "DISPLAYDXE LOGO:" + std::to_string(logo_ms) + "MS";
        out->stages.push_back({"UEFI", udet, uefi_start_ms, uefi_total_ms, false});
        out->events.push_back({uefi_start_ms, true, "UEFI START"});
        if (logo_ms > 0) {
            out->events.push_back({uefi_start_ms + std::min<int64_t>(uefi_total_ms, 200), false,
                                   "SPLASH LOGO " + std::to_string(logo_ms) + "MS"});
        }
        out->events.push_back({uefi_start_ms + uefi_total_ms, false,
                               "UEFI CORE DONE " + std::to_string(uefi_total_ms) + "MS"});
    }

    int64_t abl_start = (uefi_start_ms >= 0 && uefi_total_ms > 0)
        ? (uefi_start_ms + uefi_total_ms)
        : (sbl1_end_us > 0 ? (int64_t)llround(sbl1_end_us / 1000.0) : 0);
    if (ebs_ms > abl_start) {
        std::string adet;
        if (boot_img_ms >= 0) adet += "KL:" + std::to_string(boot_img_ms);
        if (vboot_ms >= 0) adet += (adet.empty() ? "" : " ") + std::string("VDR:") + std::to_string(vboot_ms);
        if (overlay_ms >= 0) adet += (adet.empty() ? "" : " ") + std::string("OVL:") + std::to_string(overlay_ms);
        if (dt_ms >= 0) adet += (adet.empty() ? "" : " ") + std::string("DT:") + std::to_string(dt_ms);
        if (adet.empty()) adet = "ABL IMAGE LOAD + DT";
        out->stages.push_back({"ABL", adet, abl_start, ebs_ms - abl_start, false});
        out->events.push_back({abl_start, true, "ABL LINUXLOADER"});
        if (boot_img_ms >= 0) {
            out->events.push_back({abl_start + std::max<int64_t>(20, boot_img_ms), false,
                                   "LOAD BOOT " + std::to_string(boot_img_ms) + "MS VDR "
                                   + std::to_string(std::max<int64_t>(0, vboot_ms)) + "MS"});
        }
        if (overlay_ms >= 0 || dt_ms >= 0) {
            int64_t ev_t = abl_start + (ebs_ms - abl_start) / 2;
            out->events.push_back({ev_t, false,
                                   "OVL " + std::to_string(std::max<int64_t>(0, overlay_ms))
                                   + "MS  DT " + std::to_string(std::max<int64_t>(0, dt_ms)) + "MS"});
        }
        if (vbmeta_ms >= 0 || dtbo_ms >= 0 || init_boot_ms >= 0) {
            out->events.push_back({ebs_ms - 10, false,
                                   "VBMETA " + std::to_string(std::max<int64_t>(0, vbmeta_ms))
                                   + " DTBO " + std::to_string(std::max<int64_t>(0, dtbo_ms))
                                   + " INITB " + std::to_string(std::max<int64_t>(0, init_boot_ms))});
        }
        out->events.push_back({ebs_ms, true, "EXIT BOOT SERVICES"});
    }

    int64_t total = (ebs_ms > 0) ? ebs_ms : ((uefi_start_ms >= 0 && uefi_total_ms > 0)
        ? (uefi_start_ms + uefi_total_ms)
        : (int64_t)llround(sbl1_end_us / 1000.0));
    out->total_ms = total;
    out->timeline_ms = total;
    out->summary_line.clear();

    for (const auto& s : out->stages) {
        append_summary_tok(&out->summary_line, s.id + ":" + std::to_string(s.dur_ms));
    }
    if (out->summary_line.empty()) out->summary_line = "UEFI BL:" + std::to_string(total) + "MS";

    if (!reset_reason.empty()) append_meta_tok(&out->meta_line, "POR:" + reset_reason);
    if (!cold_boot.empty()) append_meta_tok(&out->meta_line, cold_boot);
    if (!boot_dev.empty()) append_meta_tok(&out->meta_line, boot_dev);
    if (ddr_freq_mhz > 0) append_meta_tok(&out->meta_line, "DDR:" + std::to_string(ddr_freq_mhz) + "M");
    if (!slot.empty()) append_meta_tok(&out->meta_line, "SLOT:" + slot);

    sort_bl_events(&out->events);
    return !out->stages.empty();
}

bool build_qcom_kpi_profile(const QcomKpi& k, BootloaderProfile* out) {
    *out = BootloaderProfile{};
    if (!k.has_start || !k.has_end || k.bl_end < k.bl_start || !k.has_freq || k.mpm_freq == 0) {
        return false;
    }

    out->source = "QCOM_KPI";
    double freq = (double)k.mpm_freq;
    int64_t pre_bl_ms = (int64_t)llround(((double)k.bl_start * 1000.0) / freq);
    int64_t abl_ms = (int64_t)llround(((double)(k.bl_end - k.bl_start) * 1000.0) / freq);
    int64_t bl_end_ms = (int64_t)llround(((double)k.bl_end * 1000.0) / freq);

    int64_t kl_ms = -1;
    int64_t abl_pre_ms = -1;
    if (k.has_load) {
        if (k.bl_load_kernel >= k.bl_start && k.bl_load_kernel <= k.bl_end) {
            abl_pre_ms = (int64_t)llround(((double)(k.bl_load_kernel - k.bl_start) * 1000.0) / freq);
            kl_ms = (int64_t)llround(((double)(k.bl_end - k.bl_load_kernel) * 1000.0) / freq);
        } else if (k.bl_load_kernel > 0 && k.bl_load_kernel <= (k.bl_end - k.bl_start)) {
            kl_ms = (int64_t)llround(((double)k.bl_load_kernel * 1000.0) / freq);
            abl_pre_ms = std::max<int64_t>(0, abl_ms - kl_ms);
        }
    }

    if (pre_bl_ms > 0) {
        out->stages.push_back({"PRE_BL", "PBL + SBL + UEFI CORE", 0, pre_bl_ms, false});
        out->events.push_back({0, true, "POR / PRE-BL START"});
        out->events.push_back({pre_bl_ms, true, "ABL ENTRY " + std::to_string(pre_bl_ms) + "MS"});
    } else {
        out->events.push_back({0, true, "BL START"});
    }

    if (kl_ms >= 0 && abl_pre_ms >= 0) {
        out->stages.push_back({"ABL", "ABL INIT + DT", pre_bl_ms, abl_pre_ms, false});
        out->stages.push_back({"KL", "KERNEL IMAGE LOAD", pre_bl_ms + abl_pre_ms, kl_ms, false});
        out->events.push_back({pre_bl_ms + abl_pre_ms, false, "LOAD KERNEL " + std::to_string(kl_ms) + "MS"});
    } else {
        out->stages.push_back({"ABL", "ABL BOOTLOADER", pre_bl_ms, abl_ms, false});
    }
    out->events.push_back({pre_bl_ms > 0 ? bl_end_ms : abl_ms, true, "BL END -> KERNEL"});

    int64_t total = (pre_bl_ms > 0) ? bl_end_ms : abl_ms;
    out->total_ms = total;
    out->timeline_ms = total;

    if (pre_bl_ms > 0) {
        char buf[64];
        if (kl_ms >= 0) {
            snprintf(buf, sizeof(buf), "QCOM PRE:%lld ABL:%lld KL:%lldMS",
                     (long long)pre_bl_ms, (long long)abl_pre_ms, (long long)kl_ms);
        } else {
            snprintf(buf, sizeof(buf), "QCOM PRE:%lld ABL:%lld F:%llu",
                     (long long)pre_bl_ms, (long long)abl_ms, (unsigned long long)k.mpm_freq);
        }
        out->summary_line = buf;
    } else {
        char buf[64];
        if (kl_ms >= 0) {
            snprintf(buf, sizeof(buf), "QCOM KPI PRE:%lldMS KL:%lldMS",
                     (long long)abl_pre_ms, (long long)kl_ms);
        } else {
            snprintf(buf, sizeof(buf), "QCOM KPI BL:%lldMS F:%llu",
                     (long long)abl_ms, (unsigned long long)k.mpm_freq);
        }
        out->summary_line = buf;
    }

    if (k.has_kernel && k.kernel_mpm > k.bl_end) {
        int64_t handoff_ms = (int64_t)llround(((double)(k.kernel_mpm - k.bl_end) * 1000.0) / freq);
        out->meta_line = "MPM:" + std::to_string(k.mpm_freq) + "HZ HANDOFF:" + std::to_string(handoff_ms) + "MS";
    } else {
        out->meta_line = "MPM:" + std::to_string(k.mpm_freq) + "HZ";
    }
    sort_bl_events(&out->events);
    return true;
}

bool merge_qcom_kpi_into_uefilog(const QcomKpi& k, BootloaderProfile* profile) {
    if (profile->source != "UEFILOG") return false;
    if (!k.has_end || !k.has_freq || k.mpm_freq == 0) return false;
    int64_t bl_end_ms = (int64_t)llround(((double)k.bl_end * 1000.0) / (double)k.mpm_freq);
    if (bl_end_ms <= profile->total_ms || (bl_end_ms - profile->total_ms) >= 5000) return false;
    for (const auto& s : profile->stages) {
        if (s.id == "EBS") return false;
    }
    int64_t ebs_dur = bl_end_ms - profile->total_ms;
    profile->stages.push_back({"EBS", "EBS -> KERNEL ENTRY", profile->total_ms, ebs_dur, false});
    profile->events.push_back({bl_end_ms, true, "KERNEL ENTRY MPM " + std::to_string(bl_end_ms) + "MS"});
    profile->total_ms = bl_end_ms;
    profile->timeline_ms = bl_end_ms;
    sort_bl_events(&profile->events);
    return true;
}

bool parse_mtk_bootprof_file(const std::string& path, BootloaderProfile* out) {
    *out = BootloaderProfile{};
    std::string raw;
    if (!read_bounded_text(path, 65536, &raw)) return false;

    int64_t pl_ms = -1;
    int64_t lk_ms = -1;
    for (const std::string& line : split_lines_trim(raw)) {
        if (line.find("PL Boot Time") != std::string::npos) {
            extract_last_int_after(line, ":", &pl_ms);
        } else if (line.find("LK Boot Time") != std::string::npos) {
            extract_last_int_after(line, ":", &lk_ms);
        } else if (pl_ms < 0 && icontains(line, "preloader")) {
            long long v = 0;
            if (sscanf(line.c_str(), "%lld", &v) == 1 && v >= 0) pl_ms = (int64_t)v;
        } else if (lk_ms < 0 && (icontains(line, "lk boot") || icontains(line, "little kernel"))) {
            long long v = 0;
            if (sscanf(line.c_str(), "%lld", &v) == 1 && v >= 0) lk_ms = (int64_t)v;
        }
    }
    if (pl_ms < 0 && lk_ms < 0) return false;

    out->source = "BOOTPROF";
    int64_t cur = 0;
    if (pl_ms > 0) {
        out->stages.push_back({"PL", "MTK PRELOADER", cur, pl_ms, false});
        out->events.push_back({cur, true, "PL START " + std::to_string(pl_ms) + "MS"});
        cur += pl_ms;
    }
    if (lk_ms > 0) {
        out->stages.push_back({"LK", "MTK LITTLE KERNEL", cur, lk_ms, false});
        out->events.push_back({cur, true, "LK START " + std::to_string(lk_ms) + "MS"});
        cur += lk_ms;
    }
    if (cur <= 0) return false;
    out->events.push_back({cur, true, "KERNEL HANDOFF"});
    out->total_ms = cur;
    out->timeline_ms = cur;
    char buf[64];
    snprintf(buf, sizeof(buf), "MTK PL:%lldMS LK:%lldMS",
             (long long)std::max<int64_t>(0, pl_ms),
             (long long)std::max<int64_t>(0, lk_ms));
    out->summary_line = buf;
    return true;
}

bool parse_qcom_sysfs_kpi_file(const std::string& path, BootloaderProfile* out) {
    *out = BootloaderProfile{};
    std::string raw;
    if (!read_bounded_text(path, 32768, &raw)) return false;

    QcomKpi qk;
    std::vector<std::pair<std::string, int64_t>> marks;
    for (const std::string& line : split_lines_trim(raw)) {
        unsigned long long val = 0;
        if (sscanf(line.c_str(), "KPI: Bootloader start count = %llu", &val) == 1) {
            qk.bl_start = (uint64_t)val;
            qk.has_start = true;
        } else if (sscanf(line.c_str(), "KPI: Bootloader end count = %llu", &val) == 1) {
            qk.bl_end = (uint64_t)val;
            qk.has_end = true;
        } else if (sscanf(line.c_str(), "KPI: Bootloader load kernel count = %llu", &val) == 1) {
            qk.bl_load_kernel = (uint64_t)val;
            qk.has_load = true;
        } else if (sscanf(line.c_str(), "KPI: Kernel MPM timestamp = %llu", &val) == 1) {
            qk.kernel_mpm = (uint64_t)val;
            qk.has_kernel = true;
        } else if (sscanf(line.c_str(), "KPI: Kernel MPM Clock frequency = %llu", &val) == 1) {
            qk.mpm_freq = (uint64_t)val;
            qk.has_freq = true;
        } else if (line.rfind("M -", 0) == 0) {
            size_t mid = line.rfind('-');
            if (mid != std::string::npos && mid > 3) {
                std::string label = upper_copy(trim_copy(line.substr(3, mid - 3)));
                long long ts = strtoll(trim_copy(line.substr(mid + 1)).c_str(), nullptr, 10);
                if (!label.empty() && ts >= 0) marks.emplace_back(label, (int64_t)ts);
            }
        }
    }
    if (build_qcom_kpi_profile(qk, out)) {
        out->source = "QCOM_SYSFS";
        return true;
    }
    if (marks.empty()) return false;

    out->source = "QCOM_SYSFS";
    int64_t prev = 0;
    for (size_t i = 0; i < marks.size(); i++) {
        int64_t cur = marks[i].second;
        int64_t dur = std::max<int64_t>(0, cur - prev);
        out->stages.push_back({marks[i].first, marks[i].first, prev, dur, false});
        out->events.push_back({cur, i == 0 || i + 1 == marks.size(), marks[i].first});
        prev = cur;
    }
    out->total_ms = prev;
    out->timeline_ms = prev;
    out->summary_line = "QCOM SYSFS BL:" + std::to_string(prev) + "MS";
    return true;
}

std::string collect_bootconfig_meta() {
    std::string slot = upper_copy(get_prop("ro.boot.slot_suffix"));
    std::string reason = upper_copy(get_prop("ro.boot.bootreason"));
    std::string ddr;
    std::string ufs;

    std::string bc;
    if (read_bounded_text("/proc/bootconfig", 65536, &bc)) {
        for (const std::string& line : split_lines_trim(bc)) {
            if (slot.empty() && line.find("androidboot.slot_suffix") == 0) {
                size_t q1 = line.find('"');
                size_t q2 = (q1 != std::string::npos) ? line.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 > q1 + 1) slot = upper_copy(line.substr(q1 + 1, q2 - q1 - 1));
            } else if (reason.empty() && line.find("androidboot.bootreason") == 0) {
                size_t q1 = line.find('"');
                size_t q2 = (q1 != std::string::npos) ? line.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 > q1 + 1) reason = upper_copy(line.substr(q1 + 1, q2 - q1 - 1));
            } else if (ddr.empty() && line.find("androidboot.hardware.ddr") == 0) {
                size_t q1 = line.find('"');
                size_t q2 = (q1 != std::string::npos) ? line.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 > q1 + 1) ddr = upper_copy(line.substr(q1 + 1, q2 - q1 - 1));
            } else if (ufs.empty() && line.find("androidboot.hardware.ufs") == 0) {
                size_t q1 = line.find('"');
                size_t q2 = (q1 != std::string::npos) ? line.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 > q1 + 1) ufs = upper_copy(line.substr(q1 + 1, q2 - q1 - 1));
            }
        }
    }

    std::string meta;
    if (!slot.empty()) append_meta_tok(&meta, "SLOT:" + slot);
    if (!reason.empty()) append_meta_tok(&meta, "RSN:" + reason);
    if (!ddr.empty()) append_meta_tok(&meta, "DDR:" + ddr);
    if (!ufs.empty()) append_meta_tok(&meta, "UFS:" + ufs);
    return meta;
}

BootloaderProfile collect_bootloader_profile() {
    BootloaderProfile prof;
    std::string bt = get_prop("ro.boot.boottime");
    std::string bt_ex = get_prop("ro.boot.boottime_extra");
    if (!bt.empty() || !bt_ex.empty()) {
        if (parse_boottime_props(bt, bt_ex, &prof)) {
            if (prof.meta_line.empty()) prof.meta_line = collect_bootconfig_meta();
            return prof;
        }
    }
    if (parse_qcom_uefilog_file("/dev/logfs/UefiLog0.txt", &prof)) {
        if (prof.meta_line.empty()) prof.meta_line = collect_bootconfig_meta();
        return prof;
    }
    if (parse_mtk_bootprof_file("/proc/bootprof", &prof)) {
        if (prof.meta_line.empty()) prof.meta_line = collect_bootconfig_meta();
        return prof;
    }
    if (parse_qcom_sysfs_kpi_file("/sys/kernel/boot_kpi/kpi_values", &prof)) {
        if (prof.meta_line.empty()) prof.meta_line = collect_bootconfig_meta();
        return prof;
    }
    prof.meta_line = collect_bootconfig_meta();
    return prof;
}

}  // namespace bootscope
