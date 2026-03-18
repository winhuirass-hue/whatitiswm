/**
 * wm_init.cpp  –  Display, GLX, Composite, ImGui setup
 */

#include "wm.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────
ImGuiWM::ImGuiWM(const char* display_name)
    : m_display_name(display_name)
{
    // Eight virtual desktops
    for (int i = 0; i < 8; ++i) {
        Workspace ws;
        ws.id   = i;
        ws.name = std::to_string(i + 1);
        m_workspaces.push_back(ws);
    }
}

ImGuiWM::~ImGuiWM() {}

// ─────────────────────────────────────────────────────────────
bool ImGuiWM::init()
{
    if (!setup_display())   return false;
    if (!setup_composite()) return false;
    if (!setup_glx())       return false;
    if (!setup_imgui())     return false;

    setup_ewmh();
    setup_keybinds();

    // Scan already-existing windows
    Window root2, parent;
    Window* children = nullptr;
    unsigned int nchildren = 0;
    XQueryTree(m_dpy, m_root, &root2, &parent, &children, &nchildren);
    for (unsigned int i = 0; i < nchildren; ++i) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(m_dpy, children[i], &wa) &&
            !wa.override_redirect &&
            wa.map_state == IsViewable)
        {
            manage(children[i]);
        }
    }
    if (children) XFree(children);

    m_last_frame = Clock::now();
    m_running    = true;
    return true;
}

// ─────────────────────────────────────────────────────────────
bool ImGuiWM::setup_display()
{
    m_dpy = XOpenDisplay(m_display_name);
    if (!m_dpy) {
        fprintf(stderr, "[wm] Cannot open display %s\n", m_display_name);
        return false;
    }

    // Install error handler
    XSetErrorHandler([](Display*, XErrorEvent* e) -> int {
        char buf[256];
        XGetErrorText(e->display, e->error_code, buf, sizeof(buf));
        fprintf(stderr, "[X11 error] %s (req=%d, minor=%d)\n",
                buf, e->request_code, e->minor_code);
        return 0;
    });

    m_screen = DefaultScreen(m_dpy);
    m_root   = RootWindow(m_dpy, m_screen);
    m_sw     = DisplayWidth(m_dpy, m_screen);
    m_sh     = DisplayHeight(m_dpy, m_screen);

    // Become the WM by selecting SubstructureRedirectMask
    XSelectInput(m_dpy, m_root,
        SubstructureRedirectMask |
        SubstructureNotifyMask   |
        ButtonPressMask          |
        KeyPressMask             |
        PropertyChangeMask       |
        FocusChangeMask);

    XSync(m_dpy, False);
    printf("[wm] Display %s  %dx%d\n", m_display_name, m_sw, m_sh);
    return true;
}

// ─────────────────────────────────────────────────────────────
bool ImGuiWM::setup_composite()
{
    int comp_event, comp_error;
    if (!XCompositeQueryExtension(m_dpy, &comp_event, &comp_error)) {
        fprintf(stderr, "[wm] Xcomposite not available\n");
        return false;
    }

    int major = 0, minor = 0;
    XCompositeQueryVersion(m_dpy, &major, &minor);
    if (major == 0 && minor < 3) {
        fprintf(stderr, "[wm] Need Xcomposite >= 0.3 (got %d.%d)\n", major, minor);
        return false;
    }

    if (!XDamageQueryExtension(m_dpy, &m_damage_event, &m_damage_error)) {
        fprintf(stderr, "[wm] Xdamage not available\n");
        return false;
    }

    // Redirect all sub-windows to off-screen storage
    XCompositeRedirectSubwindows(m_dpy, m_root, CompositeRedirectAutomatic);
    printf("[wm] Composite redirect enabled\n");
    return true;
}

// ─────────────────────────────────────────────────────────────
bool ImGuiWM::setup_glx()
{
    // Grab the composite overlay window so we can draw on it
    m_overlay = XCompositeGetOverlayWindow(m_dpy, m_root);

    // Allow input to pass through
    XserverRegion region = XFixesCreateRegion(m_dpy, nullptr, 0);
    XFixesSetWindowShapeRegion(m_dpy, m_overlay, ShapeInput /* =2 */, 0, 0, region);
    XFixesDestroyRegion(m_dpy, region);

    XMapWindow(m_dpy, m_overlay);
    XRaiseWindow(m_dpy, m_overlay);

    // Choose a GL visual
    static int visual_attribs[] = {
        GLX_RGBA,
        GLX_RED_SIZE,   8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE,  8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER,
        None
    };
    m_vi = glXChooseVisual(m_dpy, m_screen, visual_attribs);
    if (!m_vi) {
        fprintf(stderr, "[wm] glXChooseVisual failed\n");
        return false;
    }

    m_glx_ctx = glXCreateContext(m_dpy, m_vi, nullptr, GL_TRUE);
    if (!m_glx_ctx) {
        fprintf(stderr, "[wm] glXCreateContext failed\n");
        return false;
    }

    if (!glXMakeCurrent(m_dpy, m_overlay, m_glx_ctx)) {
        fprintf(stderr, "[wm] glXMakeCurrent failed\n");
        return false;
    }

    printf("[wm] OpenGL %s\n", glGetString(GL_VERSION));
    return true;
}

// ─────────────────────────────────────────────────────────────
bool ImGuiWM::setup_imgui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)m_sw, (float)m_sh);
    io.IniFilename = nullptr;  // don't write imgui.ini

    // Dark theme with custom colours
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding   = 6.0f;
    s.FrameRounding    = 4.0f;
    s.ScrollbarRounding= 4.0f;
    s.GrabRounding     = 4.0f;
    s.WindowBorderSize = 1.0f;
    s.FramePadding     = {8, 5};
    s.ItemSpacing      = {8, 6};

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]       = {0.10f, 0.10f, 0.13f, 0.95f};
    c[ImGuiCol_TitleBg]        = {0.08f, 0.08f, 0.12f, 1.00f};
    c[ImGuiCol_TitleBgActive]  = {0.16f, 0.29f, 0.48f, 1.00f};
    c[ImGuiCol_FrameBg]        = {0.16f, 0.16f, 0.21f, 1.00f};
    c[ImGuiCol_Button]         = {0.20f, 0.35f, 0.55f, 1.00f};
    c[ImGuiCol_ButtonHovered]  = {0.26f, 0.46f, 0.70f, 1.00f};
    c[ImGuiCol_Header]         = {0.20f, 0.35f, 0.55f, 0.70f};
    c[ImGuiCol_Border]         = {0.30f, 0.30f, 0.45f, 0.60f};

    ImGui_ImplOpenGL3_Init("#version 130");

    // Build font atlas
    ImFontConfig fcfg;
    fcfg.OversampleH = 3;
    io.Fonts->AddFontDefault(&fcfg);
    ImGui_ImplOpenGL3_CreateFontsTexture();

    return true;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::setup_ewmh()
{
    auto A = [&](const char* name) { return XInternAtom(m_dpy, name, False); };

    m_WM_PROTOCOLS              = A("WM_PROTOCOLS");
    m_WM_DELETE_WINDOW          = A("WM_DELETE_WINDOW");
    m_WM_STATE                  = A("WM_STATE");
    m_NET_WM_NAME               = A("_NET_WM_NAME");
    m_NET_WM_STATE              = A("_NET_WM_STATE");
    m_NET_WM_STATE_FULLSCREEN   = A("_NET_WM_STATE_FULLSCREEN");
    m_NET_ACTIVE_WINDOW         = A("_NET_ACTIVE_WINDOW");
    m_NET_CLIENT_LIST           = A("_NET_CLIENT_LIST");
    m_NET_CURRENT_DESKTOP       = A("_NET_CURRENT_DESKTOP");
    m_NET_NUMBER_OF_DESKTOPS    = A("_NET_NUMBER_OF_DESKTOPS");
    m_NET_WM_DESKTOP            = A("_NET_WM_DESKTOP");

    long n = (long)m_workspaces.size();
    XChangeProperty(m_dpy, m_root, m_NET_NUMBER_OF_DESKTOPS,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)&n, 1);

    long cur = 0;
    XChangeProperty(m_dpy, m_root, m_NET_CURRENT_DESKTOP,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)&cur, 1);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::setup_keybinds()
{
    auto grab = [&](unsigned mod, KeySym sym, std::function<void()> fn) {
        KeyBind kb;
        kb.mod    = mod;
        kb.sym    = sym;
        kb.action = std::move(fn);
        KeyCode kc = XKeysymToKeycode(m_dpy, sym);
        if (kc) XGrabKey(m_dpy, kc, mod, m_root, True,
                         GrabModeAsync, GrabModeAsync);
        m_keybinds.push_back(std::move(kb));
    };

    const unsigned M = Mod4Mask; // Super key

    // Super+Q → kill focused
    grab(M, XK_q, [this]{ kill_focused(); });

    // Alt+L → toggle launcher
    grab(Mod1Mask, XK_l, [this]{ m_show_launcher = !m_show_launcher; });

    // Layout switching with Alt
    grab(Mod1Mask, XK_m, [this]{
        auto& ws = current_ws();
        ws.layout = Layout::Monocle;
        apply_layout(ws);
    });
    grab(Mod1Mask, XK_t, [this]{
        auto& ws = current_ws();
        ws.layout = Layout::Tiling;
        apply_layout(ws);
    });
    grab(Mod1Mask, XK_r, [this]{
        auto& ws = current_ws();
        ws.layout = Layout::Ribbon;
        apply_layout(ws);
    });

    // Super+T → toggle tiling/floating
    grab(M, XK_t, [this]{
        auto& ws = current_ws();
        ws.layout = (ws.layout == Layout::Tiling) ? Layout::Floating : Layout::Tiling;
        apply_layout(ws);
    });

    // Super+M → monocle
    grab(M, XK_m, [this]{
        current_ws().layout = Layout::Monocle;
        apply_layout(current_ws());
    });

    // Super+F → maximize focused
    grab(M, XK_f, [this]{ if (m_focused) toggle_maximize(m_focused); });

    // Super+1..8 → switch workspace
    for (int i = 0; i < 8; ++i) {
        int idx = i;
        grab(M, XK_1 + i, [this, idx]{ switch_workspace(idx); });
    }

    // Super+Shift+1..8 → move window to workspace
    for (int i = 0; i < 8; ++i) {
        int idx = i;
        grab(M | ShiftMask, XK_1 + i, [this, idx]{
            if (m_focused) move_to_workspace(m_focused, idx);
        });
    }

    // Alt+Tab → cycle focus
    grab(Mod1Mask, XK_Tab, [this]{
        auto& ws = current_ws();
        if (ws.clients.size() < 2) return;
        size_t pos = 0;
        for (size_t i = 0; i < ws.clients.size(); ++i)
            if (ws.clients[i] == m_focused) { pos = i; break; }
        pos = (pos + 1) % ws.clients.size();
        focus(ws.clients[pos]);
    });
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::shutdown()
{
    for (auto& c : m_clients)
        composite_release_window(c);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    if (m_glx_ctx) {
        glXMakeCurrent(m_dpy, None, nullptr);
        glXDestroyContext(m_dpy, m_glx_ctx);
    }
    if (m_vi) XFree(m_vi);
    if (m_overlay) XCompositeReleaseOverlayWindow(m_dpy, m_root);
    if (m_dpy)  XCloseDisplay(m_dpy);
}
