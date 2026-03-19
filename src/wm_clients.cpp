/**
 * wm_clients.cpp  –  Client lifecycle, focus, layout
 */

#include "wm.hpp"
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <algorithm>

// ─────────────────────────────────────────────────────────────
// Internal helper: collect visible (non-minimized) Client* for a workspace.
// Returns transient raw pointers — safe only as long as m_clients is not
// mutated during the same call.
// ─────────────────────────────────────────────────────────────
std::vector<Client*> ImGuiWM::visible_clients(Workspace& ws)
{
    std::vector<Client*> out;
    out.reserve(ws.clients.size());
    for (Window w : ws.clients) {
        Client* c = find_client(w);
        if (c && !c->minimized)
            out.push_back(c);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────
Client* ImGuiWM::manage(Window w)
{
    if (find_client(w)) return nullptr; // already managed

    XWindowAttributes wa;
    if (!XGetWindowAttributes(m_dpy, w, &wa)) return nullptr;

    // FIX: push_back into std::list never reallocates existing nodes,
    // so all previously stored Client* in m_win_index remain valid.
    m_clients.push_back({});
    Client& c = m_clients.back();
    c.xwin     = w;
    c.x        = wa.x;
    c.y        = wa.y;
    c.w        = wa.width;
    c.h        = wa.height;
    c.floating = true;
    c.title    = get_window_title(w);

    // FIX: m_win_index now stores Client* (stable list node address),
    // not a vector index that shifts on erase.
    m_win_index[w] = &c;

    // ── Read Motif hints (CSD/SSD) ────────────────────────────
    {
        bool no_decors = false;
        if (read_motif_hints(w, no_decors))
            c.has_csd = no_decors;
        else
            c.has_csd = false;
    }

    // Subscribe to events
    XSelectInput(m_dpy, w,
        PropertyChangeMask |
        StructureNotifyMask |
        FocusChangeMask);

    // Composite damage
    c.damage = XDamageCreate(m_dpy, w, XDamageReportNonEmpty);

    // Bind GL texture
    composite_bind_window(c);

    // Add to current workspace (store Window ID, not pointer)
    current_ws().clients.push_back(w);

    // Apply layout if tiling
    if (current_ws().layout != Layout::Floating)
        apply_layout(current_ws());

    focus(&c);
    update_ewmh_client_list();

    printf("[wm] Managed 0x%lx \"%s\"\n", w, c.title.c_str());
    return &c;
}

// ─────────────────────────────────────────────────────────────
bool ImGuiWM::read_motif_hints(Window w, bool& no_decors)
{
    no_decors = false;

    Atom A = XInternAtom(m_dpy, "_MOTIF_WM_HINTS", False);
    Atom actual;
    int format;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;

    if (Success != XGetWindowProperty(
            m_dpy, w, A, 0, 5, False, A,
            &actual, &format, &n, &after, &data) || !data)
    {
        return false;
    }

    if (n >= 5) {
        struct MotifHints {
            uint32_t flags;
            uint32_t functions;
            uint32_t decorations;
            int32_t  input_mode;
            uint32_t status;
        };
        auto* h = reinterpret_cast<MotifHints*>(data);

        // Bit 1 in flags means the decorations field is valid.
        if ((h->flags & (1u << 1)) && h->decorations == 0u)
            no_decors = true; // client requests no server-side decorations (CSD)
    }

    XFree(data);
    return true;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::unmanage(Window w)
{
    auto it = m_win_index.find(w);
    if (it == m_win_index.end()) return;

    Client* dead = it->second;

    // FIX: clear m_focused before releasing resources so no code path
    // can dereference the pointer after this point.
    if (m_focused == dead) m_focused = nullptr;
    if (m_drag.client == dead) end_drag();

    // Remove from all workspaces (stored by Window ID — no pointer fixup needed)
    for (auto& ws : m_workspaces) {
        auto& cv = ws.clients;
        cv.erase(std::remove(cv.begin(), cv.end(), w), cv.end());
        if (ws.focused == w) ws.focused = 0;
    }

    // FIX: destroy the Damage handle before releasing GL resources
    if (dead->damage) {
        XDamageDestroy(m_dpy, dead->damage);
        dead->damage = 0;
    }

    composite_release_window(*dead);

    // FIX: erase from index first, then erase the list node.
    // Because workspaces no longer hold Client*, there are no other
    // pointers to fix up — the list erase is the single owner.
    m_win_index.erase(it);

    // Erase from std::list by iterator — O(1), no elements are moved,
    // all other Client* in m_win_index remain valid.
    for (auto lit = m_clients.begin(); lit != m_clients.end(); ++lit) {
        if (&*lit == dead) {
            m_clients.erase(lit);
            break;
        }
    }

    if (current_ws().layout != Layout::Floating)
        apply_layout(current_ws());

    // Focus next visible client on current workspace
    if (!m_focused) {
        for (Window cw : current_ws().clients) {
            Client* c = find_client(cw);
            if (c && !c->minimized) { focus(c); break; }
        }
    }

    update_ewmh_client_list();
}

// ─────────────────────────────────────────────────────────────
Client* ImGuiWM::find_client(Window w)
{
    auto it = m_win_index.find(w);
    if (it == m_win_index.end()) return nullptr;
    return it->second;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::focus(Client* c)
{
    if (m_focused == c) return;

    if (m_focused) m_focused->focused = false;

    m_focused = c;

    // FIX: update current_ws().focused regardless of whether c is null,
    // so the workspace field never goes stale.
    current_ws().focused = c ? c->xwin : 0;

    if (!c) return;

    c->focused = true;
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

    // Capture the window handle before clearing m_focused so we don't
    // dereference a potentially-about-to-be-freed pointer after XSendEvent
    // triggers a re-entrant DestroyNotify in some X implementations.
    Window target = m_focused->xwin;

    // Prefer ICCCM WM_DELETE_WINDOW
    Atom* protos = nullptr;
    int n = 0;
    if (XGetWMProtocols(m_dpy, target, &protos, &n)) {
        for (int i = 0; i < n; ++i) {
            if (protos[i] == m_WM_DELETE_WINDOW) {
                XEvent ev{};
                ev.type                 = ClientMessage;
                ev.xclient.window       = target;
                ev.xclient.message_type = m_WM_PROTOCOLS;
                ev.xclient.format       = 32;
                ev.xclient.data.l[0]    = m_WM_DELETE_WINDOW;
                ev.xclient.data.l[1]    = CurrentTime;
                XSendEvent(m_dpy, target, False, NoEventMask, &ev);
                XFree(protos);
                // FIX: clear focused immediately — the client is "gone" from
                // our perspective; unmanage() will clean up when DestroyNotify
                // arrives.  Prevents a second kill attempt on the same window.
                m_focused = nullptr;
                current_ws().focused = 0;
                return;
            }
        }
        XFree(protos);
    }

    XKillClient(m_dpy, target);
    // unmanage() will be called when we receive the DestroyNotify.
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::toggle_maximize(Client* c)
{
    if (c->maximized) {
        c->x = c->prev_x; c->y = c->prev_y;
        c->w = c->prev_w; c->h = c->prev_h;
        c->maximized = false;
        // FIX: restore to tiling if the workspace is in a tiling mode
        c->floating = (current_ws().layout == Layout::Floating);
    } else {
        c->prev_x = c->x; c->prev_y = c->y;
        c->prev_w = c->w; c->prev_h = c->h;
        const int gap = 30; // leave taskbar gap
        c->x = 0; c->y = gap;
        c->w = m_sw; c->h = m_sh - gap;
        c->maximized = true;
        // FIX: treat maximized windows as floating so layout doesn't
        // try to resize them back.
        c->floating = true;
    }
    XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
    c->dirty = true;

    // FIX: re-tile so the remaining clients fill the freed/occupied space.
    if (current_ws().layout != Layout::Floating)
        apply_layout(current_ws());
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::toggle_minimize(Client* c)
{
    c->minimized = !c->minimized;
    if (c->minimized) {
        XUnmapWindow(m_dpy, c->xwin);
        if (m_focused == c) {
            m_focused = nullptr;
            current_ws().focused = 0;
            for (Window cw : current_ws().clients) {
                Client* cl = find_client(cw);
                if (cl && !cl->minimized) { focus(cl); break; }
            }
        }
    } else {
        XMapWindow(m_dpy, c->xwin);
        focus(c);
    }

    // Re-tile so the visible clients redistribute into the freed slot.
    if (current_ws().layout != Layout::Floating)
        apply_layout(current_ws());
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::move_to_workspace(Client* c, int ws_id)
{
    if (ws_id < 0 || ws_id >= (int)m_workspaces.size()) return;
    if (ws_id == m_current_ws) return;

    // Remove from current workspace
    auto& cur = current_ws().clients;
    cur.erase(std::remove(cur.begin(), cur.end(), c->xwin), cur.end());

    // Hide on screen
    XUnmapWindow(m_dpy, c->xwin);

    // Add to target workspace
    m_workspaces[ws_id].clients.push_back(c->xwin);

    if (current_ws().layout != Layout::Floating)
        apply_layout(current_ws());

    if (m_focused == c) {
        m_focused = nullptr;
        current_ws().focused = 0;
        for (Window cw : current_ws().clients) {
            Client* cl = find_client(cw);
            if (cl && !cl->minimized) { focus(cl); break; }
        }
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::switch_workspace(int id)
{
    if (id == m_current_ws) return;

    // Hide current workspace clients
    for (Window cw : current_ws().clients) {
        Client* c = find_client(cw);
        if (c && !c->minimized)
            XUnmapWindow(m_dpy, c->xwin);
    }

    m_current_ws = id;

    // Show new workspace clients
    for (Window cw : current_ws().clients) {
        Client* c = find_client(cw);
        if (c && !c->minimized)
            XMapWindow(m_dpy, c->xwin);
    }

    // FIX: clear m_focused via focus() so current_ws().focused is updated too.
    focus(nullptr);

    // Focus the last client on the new workspace (if any)
    if (!current_ws().clients.empty()) {
        // Iterate in reverse to focus the topmost / most-recently-added client
        for (auto it = current_ws().clients.rbegin();
             it != current_ws().clients.rend(); ++it)
        {
            Client* c = find_client(*it);
            if (c && !c->minimized) { focus(c); break; }
        }
    }

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
    case Layout::Tiling:   tile_workspace(ws);    break;
    case Layout::Monocle:  monocle_workspace(ws); break;
    case Layout::Floating: /* handled individually */ break;
    case Layout::Ribbon:   ribbon_workspace(ws);  break;
    case Layout::Split:    split_workspace(ws);   break;
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::tile_workspace(Workspace& ws)
{
    const int bar = 30;
    const int gap = 4;

    // FIX: skip floating and maximized clients — they manage their own geometry.
    std::vector<Client*> tv;
    for (Window cw : ws.clients) {
        Client* c = find_client(cw);
        if (c && !c->minimized && !c->floating && !c->maximized)
            tv.push_back(c);
    }

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

    // Master (left half) + stack (right half)
    int master_w = m_sw / 2;
    int stack_n  = n - 1;

    // FIX: distribute remainder pixels to the last stack client instead of
    // discarding them.  Compute base height and the leftover separately.
    int avail    = m_sh - bar - gap * (stack_n - 1);
    int stack_h  = avail / stack_n;
    int remainder = avail - stack_h * stack_n;

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
        // Give the leftover pixels to the last stack client
        c->h = (i == n - 1) ? stack_h + remainder : stack_h;
        XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
        c->dirty = true;
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::split_workspace(Workspace& ws)
{
    const int bar = 30;
    const int gap = 4;

    std::vector<Client*> tv;
    for (Window cw : ws.clients) {
        Client* c = find_client(cw);
        if (c && !c->minimized && !c->floating && !c->maximized)
            tv.push_back(c);
    }

    int n = (int)tv.size();
    if (n == 0) return;

    int avail     = m_sh - bar - gap * (n - 1);
    int h         = avail / n;
    int remainder = avail - h * n;
    int y         = bar;

    for (int i = 0; i < n; ++i) {
        Client* c = tv[i];
        c->x = gap;
        c->y = y;
        c->w = m_sw - 2 * gap;
        c->h = (i == n - 1) ? h + remainder : h;

        XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
        c->dirty = true;

        y += c->h + gap;
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::monocle_workspace(Workspace& ws)
{
    const int bar = 30;
    for (Window cw : ws.clients) {
        Client* c = find_client(cw);
        if (!c || c->minimized || c->floating || c->maximized) continue;
        c->x = 0; c->y = bar;
        c->w = m_sw; c->h = m_sh - bar;
        XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
        c->dirty = true;
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::ribbon_workspace(Workspace& ws)
{
    const int bar = 30;
    const int gap = 4;

    std::vector<Client*> tv;
    for (Window cw : ws.clients) {
        Client* c = find_client(cw);
        if (c && !c->minimized && !c->floating && !c->maximized)
            tv.push_back(c);
    }

    int n = (int)tv.size();
    if (n == 0) return;

    int avail     = m_sw - (n + 1) * gap;
    int w         = avail / n;
    int remainder = avail - w * n;
    int h         = m_sh - bar - gap;
    int x         = gap;

    for (int i = 0; i < n; ++i) {
        Client* c = tv[i];
        c->x = x;
        c->y = bar + gap;
        c->w = (i == n - 1) ? w + remainder : w;
        c->h = h;
        XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
        c->dirty = true;
        x += c->w + gap;
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::update_ewmh_client_list()
{
    // _NET_CLIENT_LIST must list all managed windows across all workspaces
    std::vector<Window> wins;
    wins.reserve(m_clients.size());
    for (const auto& c : m_clients)
        wins.push_back(c.xwin);

    XChangeProperty(m_dpy, m_root, m_NET_CLIENT_LIST,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char*)wins.data(), (int)wins.size());
}

// ─────────────────────────────────────────────────────────────
std::string ImGuiWM::get_window_title(Window w)
{
    // Try _NET_WM_NAME first (UTF-8).
    // FIX: length parameter is in 32-bit units; divide byte budget by 4.
    Atom actual; int fmt; unsigned long n, rem;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(m_dpy, w, m_NET_WM_NAME,
            0, 256 /* 256 × 4 = 1024 bytes */, False,
            XInternAtom(m_dpy, "UTF8_STRING", False),
            &actual, &fmt, &n, &rem, &data) == Success && data)
    {
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
