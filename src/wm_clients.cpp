/**
 * wm_clients.cpp  –  Client lifecycle, focus, layout
 */

#include "wm.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>

// ─────────────────────────────────────────────────────────────
Client* ImGuiWM::manage(Window w)
{
    if (find_client(w)) return nullptr; // already managed

    XWindowAttributes wa;
    if (!XGetWindowAttributes(m_dpy, w, &wa)) return nullptr;

    m_clients.push_back({});
    Client& c = m_clients.back();
    c.xwin     = w;
    c.x        = wa.x;
    c.y        = wa.y;
    c.w        = wa.width;
    c.h        = wa.height;
    c.floating = true;
    c.title    = get_window_title(w);

    m_win_index[w] = m_clients.size() - 1;

    // Listen to window events
    XSelectInput(m_dpy, w,
        PropertyChangeMask |
        StructureNotifyMask |
        FocusChangeMask);

    // Composite damage
    c.damage = XDamageCreate(m_dpy, w, XDamageReportNonEmpty);

    // Bind GL texture
    composite_bind_window(c);

    // Add to current workspace
    current_ws().clients.push_back(&c);

    // Apply layout if tiling
    if (current_ws().layout != Layout::Floating)
        apply_layout(current_ws());

    focus(&c);
    update_ewmh_client_list();

    printf("[wm] Managed 0x%lx \"%s\"\n", w, c.title.c_str());
    return &c;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::unmanage(Window w)
{
    auto it = m_win_index.find(w);
    if (it == m_win_index.end()) return;

    size_t idx = it->second;
    Client* dead = &m_clients[idx];

    // Remove from workspace
    for (auto& ws : m_workspaces) {
        auto& cv = ws.clients;
        cv.erase(std::remove(cv.begin(), cv.end(), dead), cv.end());
        if (ws.focused == dead) ws.focused = nullptr;
    }
    if (m_focused == dead) m_focused = nullptr;

    composite_release_window(*dead);

    // Rebuild index
    m_win_index.erase(it);
    m_clients.erase(m_clients.begin() + idx);

    // Re-index remaining
    m_win_index.clear();
    for (size_t i = 0; i < m_clients.size(); ++i)
        m_win_index[m_clients[i].xwin] = i;

    if (current_ws().layout != Layout::Floating)
        apply_layout(current_ws());

    // Focus next
    if (!m_focused && !current_ws().clients.empty())
        focus(current_ws().clients.back());

    update_ewmh_client_list();
}

// ─────────────────────────────────────────────────────────────
Client* ImGuiWM::find_client(Window w)
{
    auto it = m_win_index.find(w);
    if (it == m_win_index.end()) return nullptr;
    return &m_clients[it->second];
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::focus(Client* c)
{
    if (m_focused == c) return;
    if (m_focused) m_focused->focused = false;
    m_focused = c;
    if (!c) return;
    c->focused = true;
    current_ws().focused = c;
    XSetInputFocus(m_dpy, c->xwin, RevertToPointerRoot, CurrentTime);

    long active = (long)c->xwin;
    XChangeProperty(m_dpy, m_root, m_NET_ACTIVE_WINDOW,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char*)&active, 1);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::raise(Client* c)
{
    XRaiseWindow(m_dpy, c->xwin);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::kill_focused()
{
    if (!m_focused) return;

    // Prefer ICCCM WM_DELETE_WINDOW
    Atom* protos = nullptr;
    int n = 0;
    if (XGetWMProtocols(m_dpy, m_focused->xwin, &protos, &n)) {
        for (int i = 0; i < n; ++i) {
            if (protos[i] == m_WM_DELETE_WINDOW) {
                XEvent ev{};
                ev.type                 = ClientMessage;
                ev.xclient.window       = m_focused->xwin;
                ev.xclient.message_type = m_WM_PROTOCOLS;
                ev.xclient.format       = 32;
                ev.xclient.data.l[0]    = m_WM_DELETE_WINDOW;
                ev.xclient.data.l[1]    = CurrentTime;
                XSendEvent(m_dpy, m_focused->xwin, False, NoEventMask, &ev);
                if (protos) XFree(protos);
                return;
            }
        }
        if (protos) XFree(protos);
    }

    XKillClient(m_dpy, m_focused->xwin);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::toggle_maximize(Client* c)
{
    if (c->maximized) {
        c->x = c->prev_x; c->y = c->prev_y;
        c->w = c->prev_w; c->h = c->prev_h;
        c->maximized = false;
    } else {
        c->prev_x = c->x; c->prev_y = c->y;
        c->prev_w = c->w; c->prev_h = c->h;
        const int gap = 30; // leave taskbar gap
        c->x = 0; c->y = gap;
        c->w = m_sw; c->h = m_sh - gap;
        c->maximized = true;
    }
    XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
    c->dirty = true;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::toggle_minimize(Client* c)
{
    c->minimized = !c->minimized;
    if (c->minimized) {
        XUnmapWindow(m_dpy, c->xwin);
        if (m_focused == c) {
            m_focused = nullptr;
            auto& ws = current_ws();
            for (auto* cl : ws.clients)
                if (!cl->minimized) { focus(cl); break; }
        }
    } else {
        XMapWindow(m_dpy, c->xwin);
        focus(c);
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::move_to_workspace(Client* c, int ws_id)
{
    if (ws_id < 0 || ws_id >= (int)m_workspaces.size()) return;
    if (ws_id == m_current_ws) return;

    // Remove from current
    auto& cur = current_ws().clients;
    cur.erase(std::remove(cur.begin(), cur.end(), c), cur.end());

    // Hide on screen
    XUnmapWindow(m_dpy, c->xwin);

    // Add to target
    m_workspaces[ws_id].clients.push_back(c);

    if (current_ws().layout != Layout::Floating)
        apply_layout(current_ws());

    if (m_focused == c) {
        m_focused = nullptr;
        for (auto* cl : current_ws().clients)
            if (!cl->minimized) { focus(cl); break; }
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::switch_workspace(int id)
{
    if (id == m_current_ws) return;

    // Hide current
    for (auto* c : current_ws().clients)
        if (!c->minimized)
            XUnmapWindow(m_dpy, c->xwin);

    m_current_ws = id;

    // Show new
    for (auto* c : current_ws().clients)
        if (!c->minimized)
            XMapWindow(m_dpy, c->xwin);

    m_focused = nullptr;
    if (!current_ws().clients.empty())
        focus(current_ws().clients.back());

    long cur = id;
    XChangeProperty(m_dpy, m_root, m_NET_CURRENT_DESKTOP,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)&cur, 1);
}

// ─────────────────────────────────────────────────────────────
// Layout
// ─────────────────────────────────────────────────────────────
void ImGuiWM::apply_layout(Workspace& ws) {
    switch (ws.layout) {
    case Layout::Tiling:   tile_workspace(ws); break;
    case Layout::Monocle:  monocle_workspace(ws); break;
    case Layout::Floating: /* floating handled individually */ break;
    case Layout::Ribbon:   ribbon_workspace(ws); break;
    }
}


void ImGuiWM::tile_workspace(Workspace& ws)
{
    const int bar   = 30;
    const int gap   = 4;
    std::vector<Client*> tv;
    for (auto* c : ws.clients)
        if (!c->minimized) tv.push_back(c);

    int n = (int)tv.size();
    if (n == 0) return;

    if (n == 1) {
        Client* c = tv[0];
        c->x = 0; c->y = bar;
        c->w = m_sw; c->h = m_sh - bar;
        XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
        c->dirty = true;
        return;
    }

    // Horizontal ribbon layout
    int w = (m_sw - (n + 1) * gap) / n;   // width per client
    int h = m_sh - bar - gap;             // full height minus bar/gap
    int x = gap;

    for (auto* c : tv) {
        c->x = x;
        c->y = bar + gap;
        c->w = w;
        c->h = h;
        XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
        c->dirty = true;
        x += w + gap;
    }
}


    // Master (left half) + stack (right half)
    int master_w = m_sw / 2;
    int stack_n  = n - 1;
    int stack_h  = (m_sh - bar - gap * (stack_n - 1)) / stack_n;

    Client* master = tv[0];
    master->x = 0; master->y = bar;
    master->w = master_w - gap; master->h = m_sh - bar;
    XMoveResizeWindow(m_dpy, master->xwin,
                      master->x, master->y, master->w, master->h);
    master->dirty = true;

    for (int i = 1; i < n; ++i) {
        Client* c = tv[i];
        c->x = master_w;
        c->y = bar + (i - 1) * (stack_h + gap);
        c->w = m_sw - master_w;
        c->h = stack_h;
        XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
        c->dirty = true;
    }
}

void ImGuiWM::monocle_workspace(Workspace& ws)
{
    const int bar = 30;
    for (auto* c : ws.clients) {
        if (c->minimized) continue;
        c->x = 0; c->y = bar;
        c->w = m_sw; c->h = m_sh - bar;
        XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
        c->dirty = true;
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::update_ewmh_client_list()
{
    std::vector<Window> wins;
    for (auto& c : m_clients)
        wins.push_back(c.xwin);

    XChangeProperty(m_dpy, m_root, m_NET_CLIENT_LIST,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char*)wins.data(), (int)wins.size());
}

// ─────────────────────────────────────────────────────────────
std::string ImGuiWM::get_window_title(Window w)
{
    // Try _NET_WM_NAME first (UTF-8)
    Atom actual; int fmt; unsigned long n, rem;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(m_dpy, w, m_NET_WM_NAME, 0, 1024, False,
            XInternAtom(m_dpy, "UTF8_STRING", False),
            &actual, &fmt, &n, &rem, &data) == Success && data) {
        std::string s((char*)data, n);
        XFree(data);
        return s;
    }

    // Fallback to WM_NAME
    XTextProperty tp;
    if (XGetWMName(m_dpy, w, &tp) && tp.value) {
        std::string s((char*)tp.value);
        XFree(tp.value);
        return s;
    }

    return "<untitled>";
}
