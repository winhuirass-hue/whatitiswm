/**
 * wm_events.cpp  –  X11 event handling + drag/resize
 */

#include "wm.hpp"
#include "imgui.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────
void ImGuiWM::run()
{
    while (m_running) {
        process_events();
        render_frame();

        // Cap at ~60 fps
        auto now     = Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           now - m_last_frame).count();
        if (elapsed < 16667)
            usleep((unsigned)(16667 - elapsed));
        m_last_frame = Clock::now();
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::process_events()
{
    while (XPending(m_dpy)) {
        XEvent ev;
        XNextEvent(m_dpy, &ev);

        // ── Feed key/mouse state to ImGui ─────────────────────
        if (ev.type == KeyPress || ev.type == KeyRelease) {
            ImGuiIO& io = ImGui::GetIO();
            io.KeyCtrl  = (ev.xkey.state & ControlMask) != 0;
            io.KeyShift = (ev.xkey.state & ShiftMask)   != 0;
            io.KeyAlt   = (ev.xkey.state & Mod1Mask)    != 0;
            io.KeySuper = (ev.xkey.state & Mod4Mask)    != 0;

            char buf[8]; KeySym sym = 0;
            int len = XLookupString(&ev.xkey, buf, sizeof(buf), &sym, nullptr);
            if (ev.type == KeyPress && len > 0 &&
                buf[0] > 0x1f && buf[0] < 0x7f)
            {
                io.AddInputCharacter((ImWchar)buf[0]);
            }
        }

        if (ev.type == ButtonPress || ev.type == ButtonRelease) {
            ImGuiIO& io = ImGui::GetIO();
            io.MousePos = {(float)ev.xbutton.x_root, (float)ev.xbutton.y_root};
            // Update ImGui mouse button state
            int btn = (int)ev.xbutton.button - 1; // X11 buttons are 1-based
            if (btn >= 0 && btn < 5)
                io.MouseDown[btn] = (ev.type == ButtonPress);
        }

        if (ev.type == MotionNotify) {
            ImGui::GetIO().MousePos = {(float)ev.xmotion.x_root,
                                       (float)ev.xmotion.y_root};
        }

        // ── Dispatch to WM handlers ───────────────────────────
        if (ev.type == m_damage_event + XDamageNotify) {
            handle_damage_notify(reinterpret_cast<XDamageNotifyEvent&>(ev));
            continue; // damage is not in the switch range
        }

        switch (ev.type) {
        case MapRequest:
            handle_map_request(ev.xmaprequest);     break;
        case UnmapNotify:
            handle_unmap_notify(ev.xunmap);         break;
        case DestroyNotify:
            handle_destroy_notify(ev.xdestroywindow); break;
        case ConfigureRequest:
            handle_configure_request(ev.xconfigurerequest); break;
        case PropertyNotify:
            handle_property_notify(ev.xproperty);  break;
        case KeyPress:
            handle_key_press(ev.xkey);              break;
        case ButtonPress:
            handle_button_press(ev.xbutton);        break;
        case ButtonRelease:
            handle_button_release(ev.xbutton);      break;
        case MotionNotify:
            handle_motion_notify(ev.xmotion);       break;
        case Expose:
            handle_expose(ev.xexpose);              break;
        default: break;
        }
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_damage_notify(XDamageNotifyEvent& de)
{
    // Acknowledge the damage region so the server clears its accumulator.
    // composite_update_texture() must NOT call XDamageSubtract again —
    // doing it twice per event causes the next real damage to go unnoticed
    // for a full frame (the region is already empty when the texture reads it).
    XDamageSubtract(m_dpy, de.damage, None, None);

    Client* c = find_client(de.drawable);
    if (c) c->dirty = true;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_map_request(XMapRequestEvent& ev)
{
    XWindowAttributes wa;
    if (!XGetWindowAttributes(m_dpy, ev.window, &wa)) return;
    if (wa.override_redirect) {
        XMapWindow(m_dpy, ev.window);
        return;
    }
    manage(ev.window);
    XMapWindow(m_dpy, ev.window);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_unmap_notify(XUnmapEvent& ev)
{
    if (ev.send_event) return;  // synthetic (our own XUnmapWindow calls)
    unmanage(ev.window);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_destroy_notify(XDestroyWindowEvent& ev)
{
    unmanage(ev.window);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_configure_request(XConfigureRequestEvent& ev)
{
    Client* c = find_client(ev.window);
    if (c && !c->floating && !c->maximized) {
        // Tiled / maximized: honour size hints but ignore position —
        // the layout owns geometry for these windows.
        XWindowChanges twc{};
        twc.width  = c->w;
        twc.height = c->h;
        XConfigureWindow(m_dpy, ev.window, CWWidth | CWHeight, &twc);
        return;
    }

    XWindowChanges wc{};
    wc.x            = ev.x;
    wc.y            = ev.y;
    wc.width        = ev.width;
    wc.height       = ev.height;
    wc.border_width = ev.border_width;
    wc.sibling      = ev.above;
    wc.stack_mode   = ev.detail;
    XConfigureWindow(m_dpy, ev.window, (unsigned)ev.value_mask, &wc);

    // Sync our cached geometry for floating windows
    if (c) {
        if (ev.value_mask & CWX)      c->x = ev.x;
        if (ev.value_mask & CWY)      c->y = ev.y;
        if (ev.value_mask & CWWidth)  c->w = ev.width;
        if (ev.value_mask & CWHeight) c->h = ev.height;
        c->dirty = true;
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_property_notify(XPropertyEvent& ev)
{
    Client* c = find_client(ev.window);
    if (!c) return;
    if (ev.atom == XA_WM_NAME || ev.atom == m_NET_WM_NAME)
        c->title = get_window_title(ev.window);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_key_press(XKeyEvent& ev)
{
    KeySym sym = XLookupKeysym(&ev, 0);
    // Strip LockMask (Caps Lock) and Mod2Mask (Num Lock) before comparing
    unsigned int state = ev.state & ~(LockMask | Mod2Mask);
    for (auto& kb : m_keybinds) {
        if (kb.sym == sym && state == kb.mod) {
            kb.action();
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_button_press(XButtonEvent& ev)
{
    Client* c = find_client(ev.window);
    if (c) {
        focus(c);
        raise(c);

        // Super + Button1 → move window via X11 pointer grab
        // Super + Button3 → resize window via X11 pointer grab
        // This path handles drags that start outside the ImGui overlay
        // (e.g. right on the raw X window) and works for both CSD and SSD.
        if (ev.state & Mod4Mask) {
            bool resize = (ev.button == Button3);
            begin_drag(c, ev.x_root, ev.y_root, resize);
            XGrabPointer(m_dpy, m_root, True,
                PointerMotionMask | ButtonReleaseMask,
                GrabModeAsync, GrabModeAsync,
                None, None, CurrentTime);
        }
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_button_release(XButtonEvent& ev)
{
    if (m_drag.active) {
        end_drag();
        XUngrabPointer(m_dpy, CurrentTime);
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_motion_notify(XMotionEvent& ev)
{
    // Only called when a pointer grab is active (Super+drag path).
    // ImGui-side drags are handled inside render_clients / render_client_window.
    if (m_drag.active)
        update_drag(ev.x_root, ev.y_root);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_expose(XExposeEvent& ev)
{
    Client* c = find_client(ev.window);
    if (c) c->dirty = true;
}

// ─────────────────────────────────────────────────────────────
// Drag / resize  (canonical implementations — called from both the
// X11 event path and the ImGui render path)
// ─────────────────────────────────────────────────────────────
void ImGuiWM::begin_drag(Client* c, int root_x, int root_y, bool resize)
{
    if (!c) return;

    // Promote tiled clients to floating before dragging so they detach
    // from the layout grid.  Re-apply layout so remaining tiles expand.
    if (!c->floating) {
        c->floating = true;
        apply_layout(current_ws());
    }

    // Un-maximize if the user drags a maximized window
    if (c->maximized) {
        // Restore saved geometry so the window snaps back to a sensible size
        c->x = c->prev_x; c->y = c->prev_y;
        c->w = c->prev_w; c->h = c->prev_h;
        c->maximized = false;
        XMoveResizeWindow(m_dpy, c->xwin, c->x, c->y, c->w, c->h);
    }

    m_drag.active       = true;
    m_drag.resize       = resize;
    m_drag.client       = c;
    m_drag.start_root_x = root_x;
    m_drag.start_root_y = root_y;
    m_drag.start_cx     = c->x;
    m_drag.start_cy     = c->y;
    m_drag.start_cw     = c->w;
    m_drag.start_ch     = c->h;

    focus(c);
    raise(c);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::update_drag(int root_x, int root_y)
{
    if (!m_drag.active || !m_drag.client) return;

    Client* c  = m_drag.client;
    int     dx = root_x - m_drag.start_root_x;
    int     dy = root_y - m_drag.start_root_y;

    if (m_drag.resize) {
        c->w = std::max(100, m_drag.start_cw + dx);
        c->h = std::max(60,  m_drag.start_ch + dy);
        XResizeWindow(m_dpy, c->xwin, c->w, c->h);
    } else {
        c->x = m_drag.start_cx + dx;
        c->y = m_drag.start_cy + dy;

        // Clamp so at least a strip of titlebar stays on screen
        const int TITLEBAR_H = 22;
        const int MARGIN     = 10;
        c->x = std::max(MARGIN - c->w + MARGIN, c->x);
        c->x = std::min(m_sw  - MARGIN,          c->x);
        c->y = std::max(TITLEBAR_H,               c->y);
        c->y = std::min(m_sh  - MARGIN,           c->y);

        XMoveWindow(m_dpy, c->xwin, c->x, c->y);
    }
    c->dirty = true;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::end_drag()
{
    m_drag = {};   // zeroes active, client, and all coordinates
}
