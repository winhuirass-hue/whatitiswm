/**
 * config.cpp  –  Lua configuration loader
 *
 * Deps: lua5.4  (sudo apt install liblua5.4-dev)
 *
 * Reads config.lua (or ~/.config/imgui-wm/config.lua) and
 * applies settings to the WM at startup or on reload.
 */

#include "config.hpp"
#include <lua.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

static int   lua_int  (lua_State* L, const char* table, const char* key, int   def);
static float lua_float(lua_State* L, const char* table, const char* key, float def);
static bool  lua_bool (lua_State* L, const char* table, const char* key, bool  def);
static Color lua_color(lua_State* L, const char* table, const char* key, Color def);

// ─────────────────────────────────────────────────────────────
WMConfig load_config(const char* path)
{
    WMConfig cfg;   // filled with defaults

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    // Try explicit path first, then ~/.config/imgui-wm/config.lua,
    // then ./config.lua
    const char* candidates[3] = { path, nullptr, "config.lua" };
    char home_path[512] = {};
    if (const char* h = getenv("HOME")) {
        snprintf(home_path, sizeof(home_path),
                 "%s/.config/imgui-wm/config.lua", h);
        candidates[1] = home_path;
    }

    bool loaded = false;
    for (auto& cand : candidates) {
        if (!cand) continue;
        if (access(cand, R_OK) == 0) {
            if (luaL_dofile(L, cand) == LUA_OK) {
                printf("[config] Loaded %s\n", cand);
                loaded = true;
                break;
            } else {
                fprintf(stderr, "[config] Error in %s: %s\n",
                        cand, lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
    }

    if (!loaded) {
        printf("[config] No config file found, using defaults\n");
        lua_close(L);
        return cfg;
    }

    // ── wm table ─────────────────────────────────────────────
    cfg.border_width        = lua_int  (L, "wm", "border_width",        cfg.border_width);
    cfg.gap                 = lua_int  (L, "wm", "gap",                 cfg.gap);
    cfg.taskbar_height      = lua_int  (L, "wm", "taskbar_height",      cfg.taskbar_height);
    cfg.font_size           = lua_float(L, "wm", "font_size",           cfg.font_size);
    cfg.master_ratio        = lua_float(L, "wm", "master_ratio",        cfg.master_ratio);
    cfg.num_workspaces      = lua_int  (L, "wm", "workspaces",          cfg.num_workspaces);
    cfg.focus_follows_mouse = lua_bool (L, "wm", "focus_follows_mouse", cfg.focus_follows_mouse);

    // ── theme table ──────────────────────────────────────────
    cfg.theme.bg             = lua_color(L, "theme", "bg",             cfg.theme.bg);
    cfg.theme.bar_bg         = lua_color(L, "theme", "bar_bg",         cfg.theme.bar_bg);
    cfg.theme.window_bg      = lua_color(L, "theme", "window_bg",      cfg.theme.window_bg);
    cfg.theme.title_active   = lua_color(L, "theme", "title_active",   cfg.theme.title_active);
    cfg.theme.title_inactive = lua_color(L, "theme", "title_inactive", cfg.theme.title_inactive);
    cfg.theme.border_active  = lua_color(L, "theme", "border_active",  cfg.theme.border_active);
    cfg.theme.border_inactive= lua_color(L, "theme", "border_inactive",cfg.theme.border_inactive);
    cfg.theme.btn_close      = lua_color(L, "theme", "btn_close",      cfg.theme.btn_close);
    cfg.theme.btn_max        = lua_color(L, "theme", "btn_max",        cfg.theme.btn_max);
    cfg.theme.btn_min        = lua_color(L, "theme", "btn_min",        cfg.theme.btn_min);
    cfg.theme.text           = lua_color(L, "theme", "text",           cfg.theme.text);

    // ── workspace_names ──────────────────────────────────────
    lua_getglobal(L, "workspace_names");
    if (lua_istable(L, -1)) {
        for (int i = 1; i <= cfg.num_workspaces; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1))
                cfg.workspace_names.push_back(lua_tostring(L, -1));
            else
                cfg.workspace_names.push_back(std::to_string(i));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    // Fill missing names
    while ((int)cfg.workspace_names.size() < cfg.num_workspaces)
        cfg.workspace_names.push_back(
            std::to_string(cfg.workspace_names.size() + 1));

    // ── keys table ───────────────────────────────────────────
    lua_getglobal(L, "keys");
    if (lua_istable(L, -1)) {
        int n = (int)lua_rawlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

            KeyBinding kb;

            lua_getfield(L, -1, "mod");
            if (lua_isstring(L, -1)) kb.mod = lua_tostring(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "key");
            if (lua_isstring(L, -1)) kb.key = lua_tostring(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "action");
            if (lua_isstring(L, -1))
                kb.action = lua_tostring(L, -1);
            else if (lua_isfunction(L, -1)) {
                // Store reference for later call
                kb.lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);
                lua_pushnil(L); // push nil so pop below is balanced
            }
            lua_pop(L, 1);

            cfg.keys.push_back(kb);
            lua_pop(L, 1); // pop binding table
        }
    }
    lua_pop(L, 1);

    // ── mouse table ──────────────────────────────────────────
    lua_getglobal(L, "mouse");
    if (lua_istable(L, -1)) {
        int n = (int)lua_rawlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

            MouseBinding mb;
            lua_getfield(L, -1, "mod");
            if (lua_isstring(L, -1)) mb.mod = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "button");
            if (lua_isstring(L, -1)) mb.button = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "action");
            if (lua_isstring(L, -1)) mb.action = lua_tostring(L, -1);
            lua_pop(L, 1);

            cfg.mouse.push_back(mb);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // ── autostart ────────────────────────────────────────────
    lua_getglobal(L, "autostart");
    if (lua_istable(L, -1)) {
        int n = (int)lua_rawlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1))
                cfg.autostart.push_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // ── rules ────────────────────────────────────────────────
    lua_getglobal(L, "rules");
    if (lua_istable(L, -1)) {
        int n = (int)lua_rawlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

            WindowRule wr;

            lua_getfield(L, -1, "match");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "class");
                if (lua_isstring(L, -1)) wr.match_class = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "title");
                if (lua_isstring(L, -1)) wr.match_title = lua_tostring(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);

            lua_getfield(L, -1, "action");
            if (lua_isstring(L, -1)) wr.action = lua_tostring(L, -1);
            lua_pop(L, 1);

            cfg.rules.push_back(wr);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_close(L);
    return cfg;
}

// ─────────────────────────────────────────────────────────────
// Run autostart commands
// ─────────────────────────────────────────────────────────────
void run_autostart(const WMConfig& cfg)
{
    for (auto& cmd : cfg.autostart) {
        if (fork() == 0) {
            setsid();
            execlp("sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(1);
        }
        printf("[autostart] %s\n", cmd.c_str());
    }
}

// ─────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────
static int lua_int(lua_State* L, const char* table, const char* key, int def)
{
    lua_getglobal(L, table);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return def; }
    lua_getfield(L, -1, key);
    int v = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : def;
    lua_pop(L, 2);
    return v;
}

static float lua_float(lua_State* L, const char* table, const char* key, float def)
{
    lua_getglobal(L, table);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return def; }
    lua_getfield(L, -1, key);
    float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def;
    lua_pop(L, 2);
    return v;
}

static bool lua_bool(lua_State* L, const char* table, const char* key, bool def)
{
    lua_getglobal(L, table);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return def; }
    lua_getfield(L, -1, key);
    bool v = lua_isboolean(L, -1) ? (bool)lua_toboolean(L, -1) : def;
    lua_pop(L, 2);
    return v;
}

static Color lua_color(lua_State* L, const char* table, const char* key, Color def)
{
    lua_getglobal(L, table);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return def; }
    lua_getfield(L, -1, key);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return def; }

    Color c = def;
    lua_rawgeti(L, -1, 1); if (lua_isinteger(L,-1)) c.r = (uint8_t)lua_tointeger(L,-1); lua_pop(L,1);
    lua_rawgeti(L, -1, 2); if (lua_isinteger(L,-1)) c.g = (uint8_t)lua_tointeger(L,-1); lua_pop(L,1);
    lua_rawgeti(L, -1, 3); if (lua_isinteger(L,-1)) c.b = (uint8_t)lua_tointeger(L,-1); lua_pop(L,1);
    lua_rawgeti(L, -1, 4); if (lua_isinteger(L,-1)) c.a = (uint8_t)lua_tointeger(L,-1); lua_pop(L,1);

    lua_pop(L, 2);
    return c;
}
