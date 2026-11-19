#include "early/sf_gui.h"
#include "early/canvas.h"
#include "early/common.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>

namespace bootscope {

bool LibGuiApi::load() {
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
    if (!scc_create) {
        scc_create = (Sp(*)(void*, const void*, uint32_t, uint32_t, int32_t, uint32_t,
                            const Sp&, const void*, uint32_t*))dlsym(
            libgui,
            "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj");
    }
    get_phys_ids = (std::vector<uint64_t>(*)())dlsym(
        libgui, "_ZN7android21SurfaceComposerClient21getPhysicalDisplayIdsEv");
    get_phys_token = (Sp(*)(uint64_t))dlsym(
        libgui, "_ZN7android21SurfaceComposerClient23getPhysicalDisplayTokenENS_17PhysicalDisplayIdE");
    if (!get_phys_token) {
        get_phys_token = (Sp(*)(uint64_t))dlsym(
            libgui, "_ZN7android21SurfaceComposerClient23getPhysicalDisplayTokenEm");
    }
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

    return ps_self && ps_start && str8_ctor && str8_dtor && lm_ctor
        && scc_default && scc_create && get_display_state && sc_get_surface
        && tx_ctor && tx_set_layer && tx_set_pos && tx_set_matrix && tx_show && tx_apply;
}

void Geometry::compute(int buf_w, int buf_h) {
    bool is_land = width > height;
    float margin_x = roundf((float)width * 0.03f);
    if (margin_x < 20.0f) margin_x = 20.0f;
    float target_w = is_land ? roundf((float)width * 0.88f) : roundf((float)width * 0.86f);
    float max_w = (float)width - margin_x * 2.0f;
    if (target_w > max_w) target_w = max_w;
    scale = target_w / (float)buf_w;
    if (scale < 0.5f) scale = 0.5f;
    float max_h_scale = ((float)height * 0.90f) / (float)buf_h;
    if (scale > max_h_scale && max_h_scale >= 0.5f) {
        scale = max_h_scale;
        target_w = roundf((float)buf_w * scale);
    }
    float lx = roundf(((float)width - target_w) * 0.5f);
    float ly = is_land
        ? roundf(((float)height - (float)buf_h * scale) * 0.48f)
        : roundf((float)height * 0.055f);
    if (ly < 16.0f) ly = 16.0f;
    if (rot_quad == 1) {
        pos_x = (float)height - ly;
        pos_y = lx;
        dsdx = 0.0f;
        dtdx = scale;
        dtdy = -scale;
        dsdy = 0.0f;
    } else if (rot_quad == 2) {
        pos_x = (float)width - lx;
        pos_y = (float)height - ly;
        dsdx = -scale;
        dtdx = 0.0f;
        dtdy = 0.0f;
        dsdy = -scale;
    } else if (rot_quad == 3) {
        pos_x = ly;
        pos_y = (float)width - lx;
        dsdx = 0.0f;
        dtdx = -scale;
        dtdy = scale;
        dsdy = 0.0f;
    } else {
        pos_x = lx;
        pos_y = ly;
        dsdx = scale;
        dtdx = 0.0f;
        dtdy = 0.0f;
        dsdy = scale;
    }
}

bool Geometry::same_transform(const Geometry& o) const {
    return rot_quad == o.rot_quad
        && fabsf(pos_x - o.pos_x) < 0.5f
        && fabsf(pos_y - o.pos_y) < 0.5f
        && fabsf(scale - o.scale) < 0.005f;
}

static bool parse_mode_prop(const char* key, int* w, int* h, int* hz) {
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

static bool sane_dim(int v) {
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
    if (orientation == 0) {
        std::string pdo = get_prop("ro.surface_flinger.primary_display_orientation");
        if (pdo == "ORIENTATION_270") {
            g.rot_quad = 1;
            int tmp = g.width;
            g.width = g.height;
            g.height = tmp;
        } else if (pdo == "ORIENTATION_90") {
            g.rot_quad = 3;
            int tmp = g.width;
            g.width = g.height;
            g.height = tmp;
        } else if (pdo == "ORIENTATION_180") {
            g.rot_quad = 2;
        }
    }
    g.compute(canvas_width(), canvas_height());
    return g;
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

}  // namespace bootscope
