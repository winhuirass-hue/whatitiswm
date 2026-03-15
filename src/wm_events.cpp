/**
 * wm_events.cpp  –  X11 event handling
 */

#include "wm.hpp"
#include "imgui.h"
#include <cstring>
#include <cstdio>
#include <unistd.h>   // fork/exec

// ─────────────────────────────────────────────────────────────
void ImGuiWM::run()
{
    while (m_running) {
        process_events();
        render_frame();

        // Cap at ~60fps
        auto now     = Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last_frame).count();
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

        // Feed raw key/mouse events to ImGui
        if (ev.type == KeyPress || ev.type == KeyRelease) {
            ImGuiIO& io = ImGui::GetIO();
            io.KeyCtrl  = (ev.xkey.state & ControlMask) != 0;
            io.KeyShift = (ev.xkey.state & ShiftMask)   != 0;
            io.KeyAlt   = (ev.xkey.state & Mod1Mask)    != 0;
            io.KeySuper = (ev.xkey.state & Mod4Mask)    != 0;

            char buf[8]; KeySym sym = 0;
            int len = XLookupString(&ev.xkey, buf, sizeof(buf), &sym, nullptr);
            if (ev.type == KeyPress) {
                if (len > 0 && buf[0] > 0x1f && buf[0] < 0x7f)
                    io.AddInputCharacter((ImWchar)buf[0]);
            }
        }

        if (ev.type == ButtonPress || ev.type == ButtonRelease) {
            ImGuiIO& io = ImGui::GetIO();
            io.MousePos = ImVec2((float)ev.xbutton.x_root, (float)ev.xbutton.y_root);
        }

        if (ev.type == MotionNotify) {
            ImGuiIO& io = ImGui::GetIO();
            io.MousePos = ImVec2((float)ev.xmotion.x_root, (float)ev.xmotion.y_root);
        }

        // Dispatch to WM handlers
        switch (ev.type) {
        case MapRequest:
            handle_map_request(ev.xmaprequest);
            break;
        case UnmapNotify:
            handle_unmap_notify(ev.xunmap);
            break;
        case DestroyNotify:
            handle_destroy_notify(ev.xdestroywindow);
            break;
        case ConfigureRequest:
            handle_configure_request(ev.xconfigurerequest);
            break;
        case PropertyNotify:
            handle_property_notify(ev.xproperty);
            break;
        case KeyPress:
            handle_key_press(ev.xkey);
            break;
        case ButtonPress:
            handle_button_press(ev.xbutton);
            break;
        case ButtonRelease:
            handle_button_release(ev.xbutton);
            break;
        case MotionNotify:
            handle_motion_notify(ev.xmotion);
            break;
        case Expose:
            handle_expose(ev.xexpose);
            break;
        default:
            if (ev.type == m_damage_event + XDamageNotify) {
                XDamageNotifyEvent* de = (XDamageNotifyEvent*)&ev;
                XDamageSubtract(m_dpy, de->damage, None, None);
                Client* c = find_client(de->drawable);
                if (c) c->dirty = true;
            }
            break;
        }
    }
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
    if (ev.send_event) return;  // synthetic (our own XUnmapWindow)
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
    if (c && !c->floating) {
        // Tiled: honour size hints but ignore position
        XWindowChanges twc{};
        twc.width  = c->w;
        twc.height = c->h;
        XConfigureWindow(m_dpy, ev.window, CWWidth | CWHeight, &twc);
        return;
    }

    XWindowChanges wc;
    wc.x            = ev.x;
    wc.y            = ev.y;
    wc.width        = ev.width;
    wc.height       = ev.height;
    wc.border_width = ev.border_width;
    wc.sibling      = ev.above;
    wc.stack_mode   = ev.detail;
    XConfigureWindow(m_dpy, ev.window, (unsigned)ev.value_mask, &wc);

    if (c) {
        c->x = ev.x; c->y = ev.y;
        c->w = ev.width; c->h = ev.height;
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
    for (auto& kb : m_keybinds) {
        if (kb.sym == sym && (ev.state & ~LockMask) == kb.mod) {
            kb.action();
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_button_press(XButtonEvent& ev)
{
    // Button1 on root → focus
    Client* c = find_client(ev.window);
    if (c) {
        focus(c);
        raise(c);

        // Super+Button1 → move, Super+Button3 → resize
        if (ev.state & Mod4Mask) {
            bool resize = (ev.button == Button3);
            begin_drag(c, ev.x_root, ev.y_root, resize);
            XGrabPointer(m_dpy, m_root, True,
                PointerMotionMask | ButtonReleaseMask,
                GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    if (ev.button >= 1 && ev.button <= 5)
        io.MouseDown[ev.button - 1] = true;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_button_release(XButtonEvent& ev)
{
    if (m_drag.active) {
        end_drag();
        XUngrabPointer(m_dpy, CurrentTime);
    }

    ImGuiIO& io = ImGui::GetIO();
    if (ev.button >= 1 && ev.button <= 5)
        io.MouseDown[ev.button - 1] = false;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::handle_motion_notify(XMotionEvent& ev)
{
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
// Drag / move / resize
// ─────────────────────────────────────────────────────────────
void ImGuiWM::begin_drag(Client* c, int rx, int ry, bool resize)
{
    m_drag.active       = true;
    m_drag.resize       = resize;
    m_drag.client       = c;
    m_drag.start_root_x = rx;
    m_drag.start_root_y = ry;
    m_drag.start_cx     = c->x;
    m_drag.start_cy     = c->y;
    m_drag.start_cw     = c->w;
    m_drag.start_ch     = c->h;
}

void ImGuiWM::update_drag(int rx, int ry)
{
    if (!m_drag.active || !m_drag.client) return;
    Client* c   = m_drag.client;
    int     dx  = rx - m_drag.start_root_x;
    int     dy  = ry - m_drag.start_root_y;

    if (m_drag.resize) {
        c->w = std::max(100, m_drag.start_cw + dx);
        c->h = std::max(60,  m_drag.start_ch + dy);
        XResizeWindow(m_dpy, c->xwin, c->w, c->h);
    } else {
        c->x = m_drag.start_cx + dx;
        c->y = m_drag.start_cy + dy;
        XMoveWindow(m_dpy, c->xwin, c->x, c->y);
    }
    c->dirty = true;
}

void ImGuiWM::end_drag()
{
    m_drag.active = false;
    m_drag.client = nullptr;
}
