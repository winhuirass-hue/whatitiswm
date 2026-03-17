-- ─────────────────────────────────────────────────────────────
--  imgui-wm  configuration file  (Lua)
--  Loaded on startup from ~/.config/imgui-wm/config.lua
--  or from ./config.lua in the working directory.
-- ─────────────────────────────────────────────────────────────

-- ── General ──────────────────────────────────────────────────
wm = {
    border_width   = 1,          -- px around tiled windows
    gap            = 4,          -- px gap between tiled windows
    taskbar_height = 28,         -- px
    font_size      = 13.0,       -- ImGui font size (pts)
    master_ratio   = 0.55,       -- fraction of screen for tiling master
    workspaces     = 8,          -- number of virtual desktops
    focus_follows_mouse = false,
}

-- ── Theme / colours (RGBA 0-255) ─────────────────────────────
theme = {
    bg              = { 10,  10,  30, 255 },   -- desktop background
    bar_bg          = { 14,  14,  22, 224 },   -- taskbar background
    window_bg       = { 25,  25,  35, 242 },   -- decoration background
    title_active    = { 50, 115, 190, 255 },   -- focused title bar
    title_inactive  = { 38,  38,  52, 255 },   -- unfocused title bar
    border_active   = { 80, 160, 255, 255 },
    border_inactive = { 55,  55,  75, 180 },
    btn_close       = {220,  70,  70, 255 },
    btn_max         = { 70, 180,  70, 255 },
    btn_min         = {220, 180,  50, 255 },
    text            = {220, 220, 220, 255 },
}

-- ── Keybindings ──────────────────────────────────────────────
-- mod keys: "super", "alt", "ctrl", "shift"
-- Combinations: "super+shift", "alt+ctrl", etc.
-- Special keys: "return", "space", "tab", "escape",
--               "F1"-"F12", "left","right","up","down"
keys = {
    -- Window management
    { mod = "super",       key = "q",      action = "kill"              },
    { mod = "super",       key = "f",      action = "maximize"          },
    { mod = "super",       key = "n",      action = "minimize"          },
    { mod = "super",       key = "space",  action = "float_toggle"      },

    -- Layout
    { mod = "super",       key = "t",      action = "layout_tiling"     },
    { mod = "super",       key = "m",      action = "layout_monocle"    },
    { mod = "super",       key = "g",      action = "layout_floating"   },

    -- Focus
    { mod = "alt",         key = "tab",    action = "focus_next"        },
    { mod = "alt+shift",   key = "tab",    action = "focus_prev"        },

    -- Launcher
    { mod = "alt",         key = "l",      action = "launcher"          },

    -- Workspaces  1-8
    { mod = "super",       key = "1",      action = "workspace 1"       },
    { mod = "super",       key = "2",      action = "workspace 2"       },
    { mod = "super",       key = "3",      action = "workspace 3"       },
    { mod = "super",       key = "4",      action = "workspace 4"       },
    { mod = "super",       key = "5",      action = "workspace 5"       },
    { mod = "super",       key = "6",      action = "workspace 6"       },
    { mod = "super",       key = "7",      action = "workspace 7"       },
    { mod = "super",       key = "8",      action = "workspace 8"       },

    -- Move window to workspace
    { mod = "super+shift", key = "1",      action = "move_to 1"         },
    { mod = "super+shift", key = "2",      action = "move_to 2"         },
    { mod = "super+shift", key = "3",      action = "move_to 3"         },
    { mod = "super+shift", key = "4",      action = "move_to 4"         },
    { mod = "super+shift", key = "5",      action = "move_to 5"         },
    { mod = "super+shift", key = "6",      action = "move_to 6"         },
    { mod = "super+shift", key = "7",      action = "move_to 7"         },
    { mod = "super+shift", key = "8",      action = "move_to 8"         },

    -- Resize with keyboard (nudge by 40px)
    { mod = "super+ctrl",  key = "right",  action = "resize_right"      },
    { mod = "super+ctrl",  key = "left",   action = "resize_left"       },
    { mod = "super+ctrl",  key = "up",     action = "resize_up"         },
    { mod = "super+ctrl",  key = "down",   action = "resize_down"       },

    -- Custom Lua callback  (function value instead of string action)
    -- { mod = "super", key = "r", action = function() wm_reload_config() end },
}

-- ── Mouse bindings ───────────────────────────────────────────
-- button: "left"(1), "middle"(2), "right"(3)
mouse = {
    { mod = "super", button = "left",  action = "move"   },
    { mod = "super", button = "right", action = "resize" },
}

-- ── Autostart ────────────────────────────────────────────────
-- Commands run once after the WM is ready.
autostart = {
    "dunst &",
    "nm-applet &",
    -- "feh --bg-scale ~/wallpaper.jpg &",
}

-- ── Window rules ─────────────────────────────────────────────
-- Match by WM_CLASS (instance, class) or title substring.
-- Actions: "float", "tile", "maximize", "workspace N", "ignore"
rules = {
    { match = { class = "mpv"        }, action = "float"       },
    { match = { class = "Gimp"       }, action = "float"       },
    { match = { class = "Pavucontrol"}, action = "float"       },
    { match = { class = "Steam"      }, action = "workspace 5" },
    { match = { class = "firefox"    }, action = "workspace 2" },
    { match = { title = "Picture-in" }, action = "float"       },
}

-- ── Workspace names (overrides numeric defaults) ──────────────
workspace_names = { "1:term", "2:web", "3:code", "4:files",
                    "5:games", "6:media", "7:chat", "8:misc" }
