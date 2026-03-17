# Whatitiswm — Dear ImGui X11 Window Manager

A compositing window manager for X11 written in C++17 that uses
**Dear ImGui** as its entire UI toolkit: window decorations, taskbar,
workspace switcher, and app launcher all render through ImGui on an
OpenGL-backed composite overlay.

## Features

| Feature | Detail |
|---|---|
| Compositor | `Xcomposite` + `Xdamage` → GLX pixmap textures |
| Layouts | Floating · Tiling (master+stack) · Monocle |
| Workspaces | 8 virtual desktops |
| Key bindings | Super+key (see table below) |
| Taskbar | Workspace buttons, window list, layout toggle, clock |
| Launcher | Alt+L → mini command bar |
| Decorations | Per-window title bar with close / max / min buttons |

## Dependencies

```
libx11  libxcomposite  libxdamage  libxrender  libxfixes
libgl   libglx  liblua5.4
```

On Debian/Ubuntu:
```sh
sudo apt install \
  libx11-dev libxcomposite-dev libxdamage-dev \
  libxrender-dev libxfixes-dev libxext-dev libgl-dev liblua5.4-dev
```
```bash
sudo apt install xserver-xephyr
```

## Build

```sh
# 1. Fetch Dear ImGui source
make imgui

# 2. Build
make -j$(nproc)

# 3. Run (in a nested Xephyr or as your real WM)
DISPLAY=:1 ./what
```

To test inside **Xephyr**:
```sh
Xephyr :1 -screen 1280x800 &
DISPLAY=:1 ./what &
DISPLAY=:1 xterm &
```

## Key Bindings

| Key | Action |
|---|---|
| `Super+Q` | Kill focused window |
| `Alt+L` | Toggle app launcher |
| `Super+F` | Toggle maximise |
| `Super+T` | Toggle tiling layout |
| `Super+M` | Monocle layout |
| `Super+1–8` | Switch workspace |
| `Super+Shift+1–8` | Move window to workspace |
| `Alt+Tab` | Cycle focus |
| `Super+Drag(LMB)` | Move window |
| `Super+Drag(RMB)` | Resize window |

## Architecture

```
main.cpp
  └─ ImGuiWM
       ├─ wm_init.cpp      X11 / GLX / Composite / ImGui setup
       ├─ wm_events.cpp    XEvent loop → WM actions + ImGui input
       ├─ wm_clients.cpp   manage/unmanage, focus, layouts, EWMH
       └─ wm_render.cpp    GLX composite textures + ImGui overlay
```

### Compositing pipeline

```
X window ──XComposite──► off-screen pixmap
                              │
              GLX_EXT_texture_from_pixmap  (or XGetImage fallback)
                              │
                         GL texture
                              │
                     ImGui::Image()  (rendered in window decoration frame)
                              │
                    glXSwapBuffers on composite overlay
```

## Notes

* Requires `Xcomposite ≥ 0.3` and `Xdamage`.
* The WM uses **reparenting** only for keygrab/focus; visual decorations
  are drawn entirely by ImGui on top of the composite overlay.
* `GLX_EXT_texture_from_pixmap` is used when available for zero-copy
  compositing; it falls back to `XGetImage` on drivers that lack it.
* EWMH atoms `_NET_WM_NAME`, `_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`,
  and `_NET_NUMBER_OF_DESKTOPS` are set for panel/dock compatibility.
