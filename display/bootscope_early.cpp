#include <android/native_window.h>
#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 680;
constexpr int kHeight = 280;
constexpr int32_t kLayer = 0x40000001;
constexpr int kStageTotal = 10;
constexpr const char* kLayerName = "BootScopeDisplay";
constexpr const char* kDisableMarker = "/metadata/vold/ksu/bootscope_disable_display";
constexpr const char* kTmpExecPath = "/tmp/bootscope_early";
constexpr const char* kPidPath = "/dev/bootscope_early.pid";
constexpr const char* kLogPath = "/dev/bootscope_early.log";

// Little-endian RGBA_8888 (0xAABBGGRR)
constexpr uint32_t kColorBg = 0xff241b10;       // #101b24
constexpr uint32_t kColorAccent = 0xffcae07b;   // #7be0ca
constexpr uint32_t kColorPrimary = 0xfff7f4ed;  // #edf4f7
constexpr uint32_t kColorMuted = 0xffd6c39e;    // #9ec3d6
constexpr uint32_t kColorTrack = 0xff42321e;    // #1e3242

constexpr const char* kFontChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./:-_=+@>?";
constexpr const char* kFontGlyphs[] = {
    "0E11111F111111", "1E11111E11111E", "0E11101010110E", "1E11111111111E",
    "1F10101E10101F", "1F10101E101010", "0E11101711110F", "1111111F111111",
    "1F04040404041F", "0702020212120C", "11121418141211", "1010101010101F",
    "111B1515111111", "11191513111111", "0E11111111110E", "1E11111E101010",
    "0E11111115120D", "1E11111E141211", "0F10100E01011E", "1F040404040404",
    "1111111111110E", "11111111110A04", "1111111515150A", "11110A040A1111",
    "11110A04040404", "1F01020408101F",
    "0E11131519110E", "040C040404040E", "0E11010204081F", "1E01010E01011E",
    "02060A121F0202", "1F10101E01011E", "0E10101E11110E", "1F010204080808",
    "0E11110E11110E", "0E11110F01010E",
    "00000000000C0C", "01020204080810", "000C0C000C0C00", "0000001F000000",
    "0000000000001F", "00001F001F0000", "0004041F040400", "0E11171517100E",
    "10080402040810", "0E110102040004"
};

struct Sp {
    void* ptr;
    Sp() : ptr(nullptr) {}
    Sp(const Sp& o) : ptr(o.ptr) {}
    ~Sp() {}
};

struct alignas(16) Blob {
    uint8_t data[1024];
};

struct LibGuiApi {
    void* libbinder = nullptr;
    void* libutils = nullptr;
    void* libgui = nullptr;

    Sp (*ps_self)() = nullptr;
    void (*ps_start)(void*) = nullptr;

    void (*str8_ctor)(void*, const char*) = nullptr;
    void (*str8_dtor)(void*) = nullptr;

    void (*lm_ctor)(void*) = nullptr;
    Sp (*scc_default)() = nullptr;
    Sp (*scc_create)(void*, const void*, uint32_t, uint32_t, int32_t, uint32_t,
                     const Sp&, const void*, uint32_t*) = nullptr;
    std::vector<uint64_t> (*get_phys_ids)() = nullptr;
    Sp (*get_phys_token)(uint64_t) = nullptr;
    Sp (*get_internal_token)() = nullptr;
    int32_t (*get_display_state)(const Sp&, void*) = nullptr;
    int32_t (*get_active_mode)(const Sp&, void*) = nullptr;
    Sp (*sc_get_surface)(void*) = nullptr;

    void (*tx_ctor)(void*) = nullptr;
    void* (*tx_set_layer)(void*, const Sp&, int32_t) = nullptr;
    void* (*tx_set_pos)(void*, const Sp&, float, float) = nullptr;
    void* (*tx_set_matrix)(void*, const Sp&, float, float, float, float) = nullptr;
    void* (*tx_show)(void*, const Sp&) = nullptr;
    void* (*tx_hide)(void*, const Sp&) = nullptr;
    int32_t (*tx_apply)(void*, bool, bool) = nullptr;

    bool load() {
        libbinder = dlopen("/system/lib64/libbinder.so", RTLD_NOW);
        libutils = dlopen("/system/lib64/libutils.so", RTLD_NOW);
        libgui = dlopen("/system/lib64/libgui.so", RTLD_NOW);
        if (!libbinder || !libutils || !libgui) return false;

        ps_self = (Sp(*)())dlsym(libbinder, "_ZN7android12ProcessState4selfEv");
        ps_start = (void(*)(void*))dlsym(libbinder, "_ZN7android12ProcessState15startThreadPoolEv");
        str8_ctor = (void(*)(void*, const char*))dlsym(libutils, "_ZN7android7String8C1EPKc");
        str8_dtor = (void(*)(void*))dlsym(libutils, "_ZN7android7String8D1Ev");
        lm_ctor = (void(*)(void*))dlsym(libgui, "_ZN7android3gui13LayerMetadataC1Ev");
        scc_default = (Sp(*)())dlsym(libgui, "_ZN7android21SurfaceComposerClient10getDefaultEv");
        scc_create = (Sp(*)(void*, const void*, uint32_t, uint32_t, int32_t, uint32_t,
                            const Sp&, const void*, uint32_t*))dlsym(
            libgui,
            "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEERKNS_3gui13LayerMetadataEPj");
        get_phys_ids = (std::vector<uint64_t>(*)())dlsym(
            libgui, "_ZN7android21SurfaceComposerClient21getPhysicalDisplayIdsEv");
        get_phys_token = (Sp(*)(uint64_t))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient23getPhysicalDisplayTokenENS_17PhysicalDisplayIdE");
        get_internal_token = (Sp(*)())dlsym(
            libgui, "_ZN7android21SurfaceComposerClient23getInternalDisplayTokenEv");
        get_display_state = (int32_t(*)(const Sp&, void*))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient15getDisplayStateERKNS_2spINS_7IBinderEEEPNS_2ui12DisplayStateE");
        get_active_mode = (int32_t(*)(const Sp&, void*))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient20getActiveDisplayModeERKNS_2spINS_7IBinderEEEPNS_2ui11DisplayModeE");
        sc_get_surface = (Sp(*)(void*))dlsym(libgui, "_ZN7android14SurfaceControl10getSurfaceEv");
        tx_ctor = (void(*)(void*))dlsym(libgui, "_ZN7android21SurfaceComposerClient11TransactionC1Ev");
        tx_set_layer = (void*(*)(void*, const Sp&, int32_t))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient11Transaction8setLayerERKNS_2spINS_14SurfaceControlEEEi");
        tx_set_pos = (void*(*)(void*, const Sp&, float, float))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient11Transaction11setPositionERKNS_2spINS_14SurfaceControlEEEff");
        tx_set_matrix = (void*(*)(void*, const Sp&, float, float, float, float))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient11Transaction9setMatrixERKNS_2spINS_14SurfaceControlEEEffff");
        tx_show = (void*(*)(void*, const Sp&))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient11Transaction4showERKNS_2spINS_14SurfaceControlEEE");
        tx_hide = (void*(*)(void*, const Sp&))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient11Transaction4hideERKNS_2spINS_14SurfaceControlEEE");
        tx_apply = (int32_t(*)(void*, bool, bool))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient11Transaction5applyEbb");

        bool has_display_lookup = (get_phys_ids && get_phys_token) || get_internal_token;
        return str8_ctor && str8_dtor && lm_ctor && scc_default && scc_create
            && has_display_lookup && get_display_state && sc_get_surface
            && tx_ctor && tx_set_layer && tx_set_pos && tx_set_matrix && tx_show && tx_apply;
    }
};

struct Geometry {
    int width = 1080;
    int height = 2340;
    int refresh_hz = 0;
    float pos_x = 38.0f;
    float pos_y = 140.0f;
    float scale = 1.0f;

    void compute() {
        int min_dim = width < height ? width : height;
        float margin_x = roundf((float)width * 0.035f);
        if (margin_x < 24.0f) margin_x = 24.0f;
        float target_w = roundf((float)min_dim * 0.62f);
        float max_w = (float)width - margin_x * 2.0f;
        if (target_w > max_w) target_w = max_w;
        scale = target_w / (float)kWidth;
        if (scale < 0.5f) scale = 0.5f;
        pos_x = margin_x;
        pos_y = roundf((float)height * 0.06f);
    }

    bool same_transform(const Geometry& o) const {
        return fabsf(pos_x - o.pos_x) < 0.5f
            && fabsf(pos_y - o.pos_y) < 0.5f
            && fabsf(scale - o.scale) < 0.005f;
    }
};

struct StageInfo {
    int step = 0;
    std::string label = "EARLY_BOOT";
    std::string raw_ms;
};

int64_t boottime_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

std::string get_prop(const char* name) {
    char buf[PROP_VALUE_MAX]{};
    int len = __system_property_get(name, buf);
    if (len <= 0) return "";
    return std::string(buf, (size_t)len);
}

std::string svc_state(const char* svc) {
    std::string key = std::string("init.svc.") + svc;
    std::string v = get_prop(key.c_str());
    if (v == "running") return "RUN";
    if (v == "stopped") return "STOP";
    if (v == "restarting") return "RST";
    if (v == "stopping") return "STPG";
    return "-";
}

bool animation_active() {
    return get_prop("init.svc.bootanim") == "running"
        && get_prop("service.bootanim.exit") != "1"
        && get_prop("sys.boot_completed") != "1";
}

bool parse_mode_prop(const char* key, int* w, int* h, int* hz) {
    std::string v = get_prop(key);
    if (v.empty()) return false;
    int pw = 0, ph = 0, phz = 0;
    if (sscanf(v.c_str(), "%dx%d@%d", &pw, &ph, &phz) >= 2 && pw > 0 && ph > 0) {
        *w = pw;
        *h = ph;
        *hz = phz;
        return true;
    }
    return false;
}

bool sane_dim(int v) {
    return v >= 240 && v <= 8192;
}

Geometry query_geometry(const LibGuiApi& gui, const Sp& display_token) {
    Geometry g;
    bool got_size = false;
    uint32_t orientation = 0;
    if (display_token.ptr) {
        uint32_t state[32]{};
        if (gui.get_display_state(display_token, state) == 0
                && sane_dim((int)state[2]) && sane_dim((int)state[3])) {
            orientation = state[1];
            g.width = (int)state[2];
            g.height = (int)state[3];
            got_size = true;
        }
        if (gui.get_active_mode) {
            uint8_t mode[256]{};
            if (gui.get_active_mode(display_token, mode) == 0) {
                int mw = (int)((uint32_t*)mode)[1];
                int mh = (int)((uint32_t*)mode)[2];
                float fps = ((float*)mode)[12];
                if (sane_dim(mw) && sane_dim(mh)) {
                    if (orientation == 1 || orientation == 3) {
                        int tmp = mw;
                        mw = mh;
                        mh = tmp;
                    }
                    g.width = mw;
                    g.height = mh;
                    got_size = true;
                }
                if (fps >= 15.0f && fps <= 480.0f) {
                    g.refresh_hz = (int)lroundf(fps);
                }
            }
        }
    }
    if (!got_size) {
        if (!parse_mode_prop("vendor.display.preferred_mode", &g.width, &g.height, &g.refresh_hz)) {
            parse_mode_prop("persist.vendor.display.primary.boot_config", &g.width, &g.height, &g.refresh_hz);
        }
    }
    g.compute();
    return g;
}

void fill_rect(uint32_t* px, int stride, int left, int top, int right, int bottom, uint32_t color) {
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > kWidth) right = kWidth;
    if (bottom > kHeight) bottom = kHeight;
    for (int y = top; y < bottom; y++) {
        uint32_t* row = px + y * stride;
        for (int x = left; x < right; x++) {
            row[x] = color;
        }
    }
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

void draw_text(uint32_t* px, int stride, const char* text, int x, int y, int scale, uint32_t color) {
    if (!text) return;
    const size_t n_chars = strlen(kFontChars);
    const size_t fallback = n_chars - 1;
    for (size_t i = 0; text[i] != '\0'; i++, x += scale * 6) {
        char ch = (char)toupper((unsigned char)text[i]);
        if (ch == ' ') continue;
        const char* found = strchr(kFontChars, ch);
        size_t idx = found ? (size_t)(found - kFontChars) : fallback;
        const char* glyph = kFontGlyphs[idx];
        for (int row = 0; row < 7; row++) {
            int bits = hex_val(glyph[row * 2]) * 16 + hex_val(glyph[row * 2 + 1]);
            for (int col = 0; col < 5; col++) {
                if ((bits & (16 >> col)) == 0) continue;
                int left = x + col * scale;
                int top = y + row * scale;
                fill_rect(px, stride, left, top, left + scale, top + scale, color);
            }
        }
    }
}

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

struct EventState {
    std::mutex mu;
    StageInfo framework_stage;
    pid_t logcat_pid = -1;
    volatile bool stop = false;
};

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
                        std::lock_guard<std::mutex> lock(st->mu);
                        st->framework_stage = next;
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
        if (!st->stop) usleep(400000);
    }
    return nullptr;
}

ANativeWindow* extract_anw(void* surf_ptr) {
    uint8_t* base = (uint8_t*)surf_ptr;
    for (size_t off = 0; off <= 64; off += 8) {
        if (*(uint32_t*)(base + off) == 0x5f776e64) {
            return (ANativeWindow*)(base + off);
        }
    }
    return nullptr;
}

void write_pid_file() {
    FILE* f = fopen(kPidPath, "w");
    if (f) {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
}

bool reexec_from_tmp_if_needed(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--in-tmp") == 0) return false;
    }
    int src = open("/proc/self/exe", O_RDONLY);
    if (src < 0) return false;
    std::string stage = std::string(kTmpExecPath) + ".tmp";
    int dst = open(stage.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dst < 0) {
        close(src);
        return false;
    }
    char buf[16384];
    ssize_t n;
    bool ok = true;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst, buf + off, (size_t)(n - off));
            if (w <= 0) {
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok) break;
    }
    if (n < 0) ok = false;
    close(src);
    if (fchmod(dst, 0755) != 0) ok = false;
    close(dst);
    if (!ok || rename(stage.c_str(), kTmpExecPath) != 0) {
        unlink(stage.c_str());
        return false;
    }

    std::vector<char*> next_argv;
    next_argv.push_back((char*)kTmpExecPath);
    for (int i = 1; i < argc; i++) next_argv.push_back(argv[i]);
    next_argv.push_back((char*)"--in-tmp");
    next_argv.push_back(nullptr);
    execv(kTmpExecPath, next_argv.data());
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    bool preview = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--preview") == 0) preview = true;
    }
    if (!preview && access(kDisableMarker, F_OK) == 0) {
        return 0;
    }
    if (!preview) {
        reexec_from_tmp_if_needed(argc, argv);
    }

    FILE* logf = preview ? stdout : fopen(kLogPath, "w");
    if (!logf) logf = stdout;
    setvbuf(logf, nullptr, _IOLBF, 0);

    if (!preview) {
        write_pid_file();
        fprintf(logf, "started_ms=%lld\n", (long long)boottime_ms());
    }

    LibGuiApi gui;
    if (!gui.load()) {
        fprintf(logf, "exit=libgui_load_failed\n");
        if (!preview) {
            unlink(kPidPath);
            unlink(kTmpExecPath);
        }
        return 1;
    }
    if (gui.ps_self && gui.ps_start) {
        Sp ps = gui.ps_self();
        if (ps.ptr) gui.ps_start(ps.ptr);
    }

    // Wait briefly for SurfaceFlinger if started right alongside bootanim.
    Sp client;
    Sp display_token;
    Sp sc;
    ANativeWindow* anw = nullptr;
    int64_t sf_deadline = boottime_ms() + 8000;
    while (boottime_ms() < sf_deadline) {
        if (!preview && (get_prop("sys.boot_completed") == "1" || get_prop("service.bootanim.exit") == "1")) {
            fprintf(logf, "exit=boot_already_completed\n");
            if (!preview) {
                unlink(kPidPath);
                unlink(kTmpExecPath);
            }
            return 0;
        }
        client = gui.scc_default();
        if (client.ptr) {
            if (gui.get_phys_ids && gui.get_phys_token) {
                auto ids = gui.get_phys_ids();
                if (!ids.empty()) {
                    display_token = gui.get_phys_token(ids[0]);
                }
            }
            if (!display_token.ptr && gui.get_internal_token) {
                display_token = gui.get_internal_token();
            }
            if (display_token.ptr) {
                Blob name{}, metadata{};
                gui.str8_ctor(&name, kLayerName);
                gui.lm_ctor(&metadata);
                Sp null_parent{};
                uint32_t hint = 0;
                sc = gui.scc_create(
                    client.ptr, &name, kWidth, kHeight, 1 /*RGBA_8888*/,
                    0x00000400 /*eOpaque*/, null_parent, &metadata, &hint);
                gui.str8_dtor(&name);
                if (sc.ptr) {
                    Sp surf = gui.sc_get_surface(sc.ptr);
                    if (surf.ptr) {
                        anw = extract_anw(surf.ptr);
                        if (anw) break;
                    }
                }
            }
        }
        usleep(100000);
    }

    if (!anw) {
        fprintf(logf, "exit=surface_unavailable\n");
        if (!preview) {
            unlink(kPidPath);
            unlink(kTmpExecPath);
        }
        return 1;
    }

    EventState ev_state;
    pthread_t ev_thread{};
    bool ev_started = (pthread_create(&ev_thread, nullptr, event_reader_thread, &ev_state) == 0);

    const char* watched[] = {"surfaceflinger", "bootanim", "zygote", "apexd", "artd", "wpa_supplicant"};
    const char* short_names[] = {"SF", "ANIM", "ZYG", "APEX", "ARTD", "WPA"};
    std::string prev_states[6];
    std::string last_transition = "WAITING TRANSITIONS";
    Geometry last_geom;
    bool has_geom = false;
    bool first = true;

    int64_t loop_start = boottime_ms();
    bool saw_bootanim = false;
    int64_t deadline = loop_start + (preview ? 4000 : 30000);
    while (boottime_ms() < deadline) {
        if (!preview) {
            if (get_prop("init.svc.bootanim") == "running") saw_bootanim = true;
            if (get_prop("service.bootanim.exit") == "1" || get_prop("sys.boot_completed") == "1") {
                fprintf(logf, "exit=animation_ended\n");
                break;
            }
            if ((saw_bootanim || boottime_ms() - loop_start > 4000) && !animation_active()) {
                fprintf(logf, "exit=animation_ended\n");
                break;
            }
        }

        Geometry geom = query_geometry(gui, display_token);
        bool geom_changed = !has_geom || !geom.same_transform(last_geom);
        if (geom_changed) {
            Blob tx{};
            gui.tx_ctor(&tx);
            if (first) gui.tx_set_layer(&tx, sc, kLayer);
            gui.tx_set_pos(&tx, sc, geom.pos_x, geom.pos_y);
            gui.tx_set_matrix(&tx, sc, geom.scale, 0.0f, 0.0f, geom.scale);
            if (first) gui.tx_show(&tx, sc);
            gui.tx_apply(&tx, false, false);

            fprintf(logf, "%s%dx%d@%d pos=%d,%d scale=%.3f\n",
                    has_geom ? "layout_changed=" : "layout=",
                    geom.width, geom.height, geom.refresh_hz,
                    (int)lroundf(geom.pos_x), (int)lroundf(geom.pos_y), geom.scale);
            last_geom = geom;
            has_geom = true;
        }

        double uptime_s = boottime_ms() / 1000.0;
        std::string cur_states[6];
        for (int i = 0; i < 6; i++) {
            cur_states[i] = svc_state(watched[i]);
            if (!first && prev_states[i] != cur_states[i]) {
                char tbuf[64];
                snprintf(tbuf, sizeof(tbuf), "+%.2fS %s %s>%s",
                         uptime_s, short_names[i], prev_states[i].c_str(), cur_states[i].c_str());
                last_transition = tbuf;
            }
            prev_states[i] = cur_states[i];
        }

        StageInfo stage;
        {
            std::lock_guard<std::mutex> lock(ev_state.mu);
            stage = ev_state.framework_stage;
        }
        if (stage.step == 0) {
            stage = infer_native_stage(cur_states[3], cur_states[2]);
        }

        ANativeWindow_Buffer buf{};
        if (ANativeWindow_lock(anw, &buf, nullptr) == 0 && buf.bits) {
            uint32_t* px = (uint32_t*)buf.bits;
            fill_rect(px, buf.stride, 0, 0, kWidth, kHeight, kColorBg);
            fill_rect(px, buf.stride, 0, 0, 8, kHeight, kColorAccent);

            draw_text(px, buf.stride, preview ? "BOOTSCOPE / PREVIEW" : "BOOTSCOPE / BOOT", 24, 24, 3, kColorAccent);

            char mode_buf[32];
            if (geom.refresh_hz > 0) {
                snprintf(mode_buf, sizeof(mode_buf), "%dX%d@%d", geom.width, geom.height, geom.refresh_hz);
            } else {
                snprintf(mode_buf, sizeof(mode_buf), "%dX%d", geom.width, geom.height);
            }
            int mode_x = kWidth - 24 - (int)strlen(mode_buf) * 18;
            if (mode_x < 384) mode_x = 384;
            draw_text(px, buf.stride, mode_buf, mode_x, 24, 3, kColorMuted);

            char meta_buf[48];
            snprintf(meta_buf, sizeof(meta_buf), "UPTIME %-7.2fS  STAGE %d/%d", uptime_s, stage.step, kStageTotal);
            draw_text(px, buf.stride, meta_buf, 24, 66, 3, kColorPrimary);

            const int bar_left = 24, bar_top = 98, bar_right = kWidth - 24, bar_bottom = 110;
            fill_rect(px, buf.stride, bar_left, bar_top, bar_right, bar_bottom, kColorTrack);
            if (stage.step > 0) {
                int filled = bar_left + (int)lroundf((bar_right - bar_left) * (stage.step / (float)kStageTotal));
                fill_rect(px, buf.stride, bar_left, bar_top, filled, bar_bottom, kColorAccent);
            }

            std::string stage_line = stage.raw_ms.empty()
                ? stage.label
                : (stage.label + "  " + stage.raw_ms + "MS");
            draw_text(px, buf.stride, stage_line.c_str(), 24, 126, 3, kColorAccent);

            char row1[48], row2[48];
            snprintf(row1, sizeof(row1), "SF:%-4s ANIM:%-4s ZYG:%-4s",
                     cur_states[0].c_str(), cur_states[1].c_str(), cur_states[2].c_str());
            snprintf(row2, sizeof(row2), "APEX:%-4s ARTD:%-4s WPA:%-4s",
                     cur_states[3].c_str(), cur_states[4].c_str(), cur_states[5].c_str());
            draw_text(px, buf.stride, row1, 24, 166, 3, kColorPrimary);
            draw_text(px, buf.stride, row2, 24, 198, 3, kColorPrimary);

            std::string footer;
            if (last_transition == "WAITING TRANSITIONS") {
                std::string bc = get_prop("sys.boot_completed");
                std::string ae = get_prop("service.bootanim.exit");
                footer = "BC=" + (bc.empty() ? std::string("0") : bc)
                    + " ANIM_EXIT=" + (ae.empty() ? std::string("0") : ae);
            } else {
                footer = "LAST " + last_transition;
            }
            if (footer.size() > 35) footer.resize(35);
            draw_text(px, buf.stride, footer.c_str(), 24, 238, 3, kColorMuted);

            ANativeWindow_unlockAndPost(anw);
        }

        if (first) {
            fprintf(logf, "first_frame_submitted_ms=%lld\n", (long long)boottime_ms());
            first = false;
        }
        usleep(250000);
    }

    if (boottime_ms() >= deadline) {
        fprintf(logf, "exit=time_limit\n");
    }

    ev_state.stop = true;
    {
        std::lock_guard<std::mutex> lock(ev_state.mu);
        if (ev_state.logcat_pid > 0) {
            kill(ev_state.logcat_pid, SIGKILL);
        }
    }
    if (ev_started) pthread_join(ev_thread, nullptr);

    if (gui.tx_hide && sc.ptr) {
        Blob tx{};
        gui.tx_ctor(&tx);
        gui.tx_hide(&tx, sc);
        gui.tx_apply(&tx, true, false);
    }
    fprintf(logf, "released_ms=%lld\n", (long long)boottime_ms());
    if (!preview) {
        unlink(kPidPath);
        unlink(kTmpExecPath);
        if (logf != stdout) fclose(logf);
    }
    return 0;
}
