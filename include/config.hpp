#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

struct Theme {
    Color bg             = {10,  10,  30, 255};
    Color bar_bg         = {14,  14,  22, 224};
    Color window_bg      = {25,  25,  35, 242};
    Color title_active   = {50, 115, 190, 255};
    Color title_inactive = {38,  38,  52, 255};
    Color border_active  = {80, 160, 255, 255};
    Color border_inactive= {55,  55,  75, 180};
    Color btn_close      = {220, 70,  70, 255};
    Color btn_max        = {70,  180, 70, 255};
    Color btn_min        = {220, 180, 50, 255};
    Color text           = {220, 220, 220, 255};
};

struct KeyBinding {
    std::string mod;
    std::string key;
    std::string action;   // empty if lua_ref is set
    int         lua_ref = -1;
};

struct MouseBinding {
    std::string mod;
    std::string button;
    std::string action;
};

struct WindowRule {
    std::string match_class;
    std::string match_title;
    std::string action;
};

// ─────────────────────────────────────────────────────────────
struct WMConfig {
    // wm table
    int   border_width        = 1;
    int   gap                 = 4;
    int   taskbar_height      = 28;
    float font_size           = 13.0f;
    float master_ratio        = 0.55f;
    int   num_workspaces      = 8;
    bool  focus_follows_mouse = false;

    Theme                    theme;
    std::vector<std::string> workspace_names;
    std::vector<KeyBinding>  keys;
    std::vector<MouseBinding>mouse;
    std::vector<std::string> autostart;
    std::vector<WindowRule>  rules;
};

// ─────────────────────────────────────────────────────────────
WMConfig load_config(const char* path = nullptr);
void     run_autostart(const WMConfig& cfg);
