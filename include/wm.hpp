#pragma once

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>       
#include <X11/cursorfont.h>   
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <GL/glx.h>
#include <GL/gl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <functional>
#include <chrono>

// ─────────────────────────────────────────────────────────────
//  Managed client window
// ─────────────────────────────────────────────────────────────
struct Client {
    Window  xwin        = 0;
    Window  frame       = 0;       // reparenting frame (optional)
    int     x = 0, y = 0;
    int     w = 800, h = 600;
    int     prev_x = 0, prev_y = 0, prev_w = 0, prev_h = 0; // restore from max
    bool    floating    = true;
    bool    maximized   = false;
    bool    has_csd     = false;
    bool    minimized   = false;
    bool    focused     = false;
    bool    dirty       = true;    // composite texture needs refresh
    std::string title;

    // Composite backing texture
    Pixmap      pixmap  = 0;
    GLXPixmap   glx_pix = 0;
    GLuint      tex     = 0;

    // Damage handle
    Damage      damage  = 0;
};

// ─────────────────────────────────────────────────────────────
//  Layout modes
// ─────────────────────────────────────────────────────────────
enum class Layout { Floating, Tiling, Monocle, Ribbon, Split };

// ─────────────────────────────────────────────────────────────
//  Workspace / virtual desktop
// ─────────────────────────────────────────────────────────────
struct Workspace {
    int              id     = 0;
    std::string      name;
    Layout           layout = Layout::Floating;
    std::vector<Client*> clients;
    Client*          focused = nullptr;
};

// ─────────────────────────────────────────────────────────────
//  Key binding
// ─────────────────────────────────────────────────────────────
struct KeyBind {
    unsigned int  mod;
    KeySym        sym;
    std::function<void()> action;
};

// ─────────────────────────────────────────────────────────────
//  The window manager
// ─────────────────────────────────────────────────────────────
class ImGuiWM {
public:
    explicit ImGuiWM(const char* display_name);
    ~ImGuiWM();

    bool init();
    void run();
    bool read_motif_hints(Window w, bool& no_decors);
    void shutdown();
    void request_quit() { m_running = false; }

private:
    // ── X11 / GLX setup ──────────────────────────────────────
    bool setup_display();
    bool setup_composite();
    bool setup_glx();
    bool setup_imgui();
    void setup_keybinds();
    void setup_ewmh();

    bool read_motif_hints(Window w, bool& no_decors);

    // ── Event loop ───────────────────────────────────────────
    void process_events();
    void handle_map_request(XMapRequestEvent& ev);
    void handle_unmap_notify(XUnmapEvent& ev);
    void handle_destroy_notify(XDestroyWindowEvent& ev);
    void handle_configure_request(XConfigureRequestEvent& ev);
    void handle_property_notify(XPropertyEvent& ev);
    void handle_key_press(XKeyEvent& ev);
    void handle_button_press(XButtonEvent& ev);
    void handle_button_release(XButtonEvent& ev);
    void handle_motion_notify(XMotionEvent& ev);
    void handle_damage_notify(XDamageNotifyEvent& ev);
    void handle_expose(XExposeEvent& ev);

    // ── Rendering ────────────────────────────────────────────
    void render_frame();
    void render_desktop();
    void render_clients();
    void render_client_window(Client& c);
    void render_client_shadow(const Client& c);
    void render_taskbar();
    void render_launcher();
    void render_workspace_switcher();
    void render_notifications();

    // ── Compositor ───────────────────────────────────────────
    void composite_bind_window(Client& c);
    void composite_release_window(Client& c);
    void composite_update_texture(Client& c);

    // ── Client management ────────────────────────────────────
    Client* manage(Window w);
    void    unmanage(Window w);
    Client* find_client(Window w);
    void    focus(Client* c);
    void    raise(Client* c);
    void    kill_focused();
    void    toggle_maximize(Client* c);
    void    toggle_minimize(Client* c);
    void    move_to_workspace(Client* c, int ws_id);

    // ── Layout ───────────────────────────────────────────────
    void apply_layout(Workspace& ws);
    void tile_workspace(Workspace& ws);
    void monocle_workspace(Workspace& ws);
    void ribbon_workspace(Workspace& ws);
    void split_workspace(Workspace& ws);

    // ── Workspaces ───────────────────────────────────────────
    void switch_workspace(int id);
    Workspace& current_ws() { return m_workspaces[m_current_ws]; }

    // ── EWMH helpers ─────────────────────────────────────────
    std::string get_window_title(Window w);
    void update_ewmh_client_list();

    // ── Drag helpers ─────────────────────────────────────────
    void begin_drag(Client* c, int root_x, int root_y, bool resize);
    void update_drag(int root_x, int root_y);
    void end_drag();

private:
    // X11
    const char*  m_display_name;
    Display*     m_dpy        = nullptr;
    int          m_screen     = 0;
    Window       m_root       = 0;
    int          m_sw = 0, m_sh = 0;   // screen dimensions

    // Composite / Damage extension event bases
    int          m_damage_event = 0;
    int          m_damage_error = 0;

    // GLX overlay window
    Window       m_overlay     = 0;
    GLXContext   m_glx_ctx     = nullptr;
    XVisualInfo* m_vi          = nullptr;

    // ImGui GL framebuffer
    GLuint       m_fbo         = 0;
    GLuint       m_fbo_tex     = 0;

    Window m_ewmh_window = 0;

    // Atoms
    Atom m_WM_PROTOCOLS, m_WM_DELETE_WINDOW, m_WM_STATE;
    Atom m_NET_WM_NAME, m_NET_WM_STATE, m_NET_WM_STATE_FULLSCREEN;
    Atom m_NET_ACTIVE_WINDOW, m_NET_CLIENT_LIST;
    Atom m_NET_CURRENT_DESKTOP, m_NET_NUMBER_OF_DESKTOPS;
    Atom m_NET_WM_DESKTOP;
    Atom m_NET_SUPPORTED = 0;
    Atom m_NET_SUPPORTING_WM_CHECK = 0;
    Atom m_UTF8_STRING = 0;

    // State
    bool         m_running     = false;
    std::vector<Client>  m_clients;
    std::unordered_map<Window, size_t> m_win_index; // xwin → index into m_clients
    std::vector<Workspace> m_workspaces;
    int          m_current_ws  = 0;
    Client*      m_focused     = nullptr;

    // Key bindings
    std::vector<KeyBind> m_keybinds;

    // Drag / resize state
    struct DragState {
        bool    active  = false;
        bool    resize  = false;
        Client* client  = nullptr;
        int     start_root_x = 0, start_root_y = 0;
        int     start_cx = 0, start_cy = 0;
        int     start_cw = 0, start_ch = 0;
    } m_drag;

    // Launcher
    bool         m_show_launcher = false;
    char         m_launcher_buf[256] = {};

    // Notification queue
    struct Notification { std::string text; float ttl; };
    std::vector<Notification> m_notifications;

    std::vector<std::string> m_launcher_history;

    // Frame timing
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_last_frame;
};
