#pragma once

#include <android/native_window.h>
#include <stdint.h>
#include <vector>

namespace bootscope {

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

    bool load();
};

struct Geometry {
    int width = 1080;
    int height = 2340;
    int refresh_hz = 0;
    int rot_quad = 0;
    float pos_x = 38.0f;
    float pos_y = 140.0f;
    float scale = 1.0f;
    float dsdx = 1.0f;
    float dtdx = 0.0f;
    float dtdy = 0.0f;
    float dsdy = 1.0f;

    void compute(int buf_w, int buf_h);
    bool same_transform(const Geometry& o) const;
};

Geometry query_geometry(const LibGuiApi& gui, const Sp& display_token);
ANativeWindow* extract_anw(void* surf_ptr);

}  // namespace bootscope
