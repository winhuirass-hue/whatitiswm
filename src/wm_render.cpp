/**
 * wm_render.cpp  –  GLX compositing + ImGui overlay rendering
 */

#include "wm.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include <GL/glx.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>   // waitpid, WNOHANG
#include <fcntl.h>      // open, O_RDWR
#include <algorithm>

// ─────────────────────────────────────────────────────────────
// We use the XGetImage fallback path exclusively.
// GLX_EXT_texture_from_pixmap is unreliable on Mesa/software
// renderers and causes segfaults; XGetImage is slower but safe.
// ─────────────────────────────────────────────────────────────

void ImGuiWM::composite_bind_window(Client& c)
{
    // Damage tracking only – no GL work here.
    // GL texture is created lazily on first composite_update_texture call.
    c.tex   = 0;
    c.dirty = true;
    // (glx_pix / pixmap stay 0 – we use XGetImage path)
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::composite_release_window(Client& c)
{
    if (c.tex) {
        glDeleteTextures(1, &c.tex);
        c.tex = 0;
    }
    // glx_pix / pixmap not used in XGetImage path
    if (c.damage) {
        XDamageDestroy(m_dpy, c.damage);
        c.damage = 0;
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::composite_update_texture(Client& c)
{
    if (!c.dirty) return;
    c.dirty = false;

    if (c.w <= 0 || c.h <= 0) return;

    // Note: XDamageSubtract is called in handle_damage_notify() (wm_events.cpp),
    // not here.  Calling it twice per event would clear the region before the
    // next real damage arrives, causing missed updates for one frame.
    XImage* img = XGetImage(m_dpy, c.xwin,
                            0, 0, c.w, c.h,
                            AllPlanes, ZPixmap);
    if (!img) return;

    // Convert X pixel format → RGBA
    std::vector<unsigned char> buf(c.w * c.h * 4);
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            unsigned long px = XGetPixel(img, x, y);
            int off = (y * c.w + x) * 4;
            buf[off + 0] = (unsigned char)((px >> 16) & 0xff); // R
            buf[off + 1] = (unsigned char)((px >>  8) & 0xff); // G
            buf[off + 2] = (unsigned char)((px >>  0) & 0xff); // B
            buf[off + 3] = 255;
        }
    }
    XDestroyImage(img);

    if (!c.tex) {
        glGenTextures(1, &c.tex);
        glBindTexture(GL_TEXTURE_2D, c.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     c.w, c.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        c.tex_w = c.w;
        c.tex_h = c.h;
    } else {
        glBindTexture(GL_TEXTURE_2D, c.tex);
        if (c.tex_w != c.w || c.tex_h != c.h) {
            // Window resized — reallocate storage
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         c.w, c.h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
            c.tex_w = c.w;
            c.tex_h = c.h;
        } else {
            // Same size — sub-update avoids GPU realloc, ~2x faster
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                            0, 0, c.w, c.h,
                            GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

// ─────────────────────────────────────────────────────────────
// Main render loop
// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_frame()
{
    // Make sure our GL context is current every frame
    glXMakeCurrent(m_dpy, m_overlay, m_glx_ctx);

    // Update dirty composite textures
    for (auto& c : m_clients)
        if (c.dirty) composite_update_texture(c);

    // New ImGui frame
    ImGuiIO& io = ImGui::GetIO();
    auto now = Clock::now();
    io.DeltaTime = std::max(0.0001f,
        std::chrono::duration<float>(now - m_last_frame).count());
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    render_desktop();
    render_clients();
    render_taskbar();

    if (m_show_launcher) render_launcher();

    render_notifications();

    ImGui::Render();

    glViewport(0, 0, m_sw, m_sh);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glXSwapBuffers(m_dpy, m_overlay);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_desktop()
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilledMultiColor(
        {0, 0}, {(float)m_sw, (float)m_sh},
        IM_COL32(10, 10, 30, 255),
        IM_COL32(10, 10, 30, 255),
        IM_COL32(20, 20, 50, 255),
        IM_COL32(20, 20, 50, 255));
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_clients()
{
    // Super + left-drag moves any window (CSD or SSD) regardless of where
    // the cursor sits inside it.  We check this once here, before iterating
    // over windows, so it also works when the cursor is inside a CSD window
    // that has no visible titlebar strip we can hit-test.
    {
        bool super_held = ImGui::GetIO().KeySuper ||
                          (ImGui::GetIO().KeyMods & ImGuiMod_Super);
        bool lmb_down   = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        bool lmb_click  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool lmb_rel    = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        ImVec2 mp = ImGui::GetMousePos();

        if (lmb_rel && m_drag.active)
            end_drag();

        if (super_held && lmb_click && !m_drag.active && m_focused) {
            // Begin move on focused window
            begin_drag(m_focused, (int)mp.x, (int)mp.y, /*resize=*/false);
        }
        if (m_drag.active && lmb_down)
            update_drag((int)mp.x, (int)mp.y);
    }

    for (Window cw : current_ws().clients) {
        Client* c = find_client(cw);
        if (!c || c->minimized) continue;

        if (c->dirty) {
            composite_update_texture(*c);
            c->dirty = false;
        }

        render_client_window(*c);
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_client_window(Client& c)
{
    const float TITLEBAR_H = 22.0f;
    // Resize handle size in the bottom-right corner
    const float RESIZE_GRIP = 12.0f;

    // For CSD windows (client draws its own decorations) we show no ImGui
    // titlebar — the window content fills the whole box.  For SSD windows
    // we prepend our own TITLEBAR_H-tall strip above the content.
    const float our_tb = c.has_csd ? 0.0f : TITLEBAR_H;

    ImVec2 win_pos  = {(float)c.x, (float)c.y - our_tb};
    ImVec2 win_size = {(float)c.w, (float)c.h + our_tb};

    ImGui::SetNextWindowPos(win_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(win_size, ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize          |
        ImGuiWindowFlags_NoScrollbar       |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings   |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (c.focused)
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.2f, 0.45f, 0.75f, 1.0f));
    else
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.15f, 0.15f, 0.20f, 1.0f));

    std::string wid = "##win_" + std::to_string(c.xwin);
    ImGui::Begin((c.title + wid).c_str(), nullptr, flags);

    ImVec2 tl = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Titlebar hit regions ──────────────────────────────────
    // Defined in screen coords.  For CSD windows our_tb==0, so the
    // titlebar rect has zero height and none of the SSD interactions fire.
    ImVec2 tb_min = tl;
    ImVec2 tb_max = {tl.x + win_size.x, tl.y + our_tb};

    // Traffic-light buttons (SSD only)
    ImVec2 close_pos = {tb_max.x - 16.0f, tl.y + 11.0f};
    ImVec2 max_pos   = {tb_max.x - 36.0f, tl.y + 11.0f};
    ImVec2 min_pos   = {tb_max.x - 56.0f, tl.y + 11.0f};

    if (!c.has_csd) {
        dl->AddCircleFilled(close_pos, 7.0f, IM_COL32(220,  70,  70, 255));
        dl->AddCircleFilled(max_pos,   7.0f, IM_COL32( 70, 180,  70, 255));
        dl->AddCircleFilled(min_pos,   7.0f, IM_COL32(220, 180,  50, 255));
    }

    // Resize grip — small triangle in the bottom-right corner, always shown
    {
        ImVec2 br = {tl.x + win_size.x, tl.y + win_size.y};
        dl->AddTriangleFilled(
            {br.x - RESIZE_GRIP, br.y},
            {br.x, br.y - RESIZE_GRIP},
            {br.x, br.y},
            IM_COL32(120, 120, 160, 100));
    }

    // ── Window content ────────────────────────────────────────
    ImVec2 content_size = {(float)c.w, (float)c.h};
    ImGui::BeginChild("##client_area", content_size, false,
                      ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoSavedSettings);

    if (c.tex) {
        ImGui::Image((ImTextureID)(intptr_t)c.tex, content_size,
                     ImVec2(0, 0), ImVec2(1, 1));
    } else {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        cdl->AddRectFilled(cp,
            {cp.x + content_size.x, cp.y + content_size.y},
            IM_COL32(30, 30, 40, 255));
        ImGui::Dummy(content_size);
    }

    // Forward right-clicks to the client window (press + release pair)
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        focus(&c);
        raise(&c);
        XFlush(m_dpy);

        const int mx = (int)ImGui::GetMousePos().x;
        const int my = (int)ImGui::GetMousePos().y;

        XEvent ev{};
        ev.xbutton.display     = m_dpy;
        ev.xbutton.window      = c.xwin;
        ev.xbutton.root        = m_root;
        ev.xbutton.subwindow   = None;
        ev.xbutton.time        = CurrentTime;
        ev.xbutton.x           = mx - c.x;
        ev.xbutton.y           = my - c.y;
        ev.xbutton.x_root      = mx;
        ev.xbutton.y_root      = my;
        ev.xbutton.button      = Button3;
        ev.xbutton.same_screen = True;
        ev.type          = ButtonPress;
        ev.xbutton.state = 0;
        XSendEvent(m_dpy, c.xwin, True, ButtonPressMask, &ev);
        ev.type          = ButtonRelease;
        ev.xbutton.state = Button3Mask;
        ev.xbutton.time  = CurrentTime;
        XSendEvent(m_dpy, c.xwin, True, ButtonReleaseMask, &ev);
        XFlush(m_dpy);
    }

    ImGui::EndChild();

    // ── Mouse interactions ────────────────────────────────────
    ImVec2 mp  = ImGui::GetMousePos();
    bool   lmb = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    bool   lmb_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool   lmb_rel  = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    auto in_circle = [](ImVec2 p, ImVec2 centre, float r) {
        float dx = p.x - centre.x, dy = p.y - centre.y;
        return dx*dx + dy*dy <= r*r;
    };
    auto in_rect = [](ImVec2 p, ImVec2 rmin, ImVec2 rmax) {
        return p.x >= rmin.x && p.x <= rmax.x &&
               p.y >= rmin.y && p.y <= rmax.y;
    };

    // Resize grip rect (bottom-right corner)
    ImVec2 br     = {tl.x + win_size.x, tl.y + win_size.y};
    ImVec2 rg_min = {br.x - RESIZE_GRIP * 2, br.y - RESIZE_GRIP * 2};
    bool   on_grip = in_rect(mp, rg_min, br);

    // Set cursor shape to hint resize / move
    if (on_grip)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if (in_rect(mp, tb_min, tb_max) && !c.has_csd)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (lmb_rel && m_drag.active)
        end_drag();

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
        // Focus + raise on any left click
        if (lmb) { focus(&c); raise(&c); }

        if (!m_drag.active) {
            // ── Start resize (bottom-right grip) ─────────────
            if (lmb && on_grip) {
                begin_drag(&c, (int)mp.x, (int)mp.y, /*resize=*/true);
            }
            // ── Start move (titlebar drag, SSD) ──────────────
            else if (lmb && !c.has_csd && in_rect(mp, tb_min, tb_max)
                     && !in_circle(mp, close_pos, 9.0f)
                     && !in_circle(mp, max_pos,   9.0f)
                     && !in_circle(mp, min_pos,   9.0f))
            {
                begin_drag(&c, (int)mp.x, (int)mp.y, /*resize=*/false);
            }
            // ── Start move (CSD titlebar: Super + left drag) ─
            // CSD windows draw their own titlebar so we can't hit-test it.
            // Convention: Super + drag moves the window regardless of where
            // the cursor is inside it, matching most compositor behaviour.
        }
    }

    // Continue drag even if cursor leaves the window this frame
    if (m_drag.active && m_drag.client == &c) {
        if (lmb_down)
            update_drag((int)mp.x, (int)mp.y);
        else
            end_drag();
    }

    // ── SSD traffic-light clicks ──────────────────────────────
    if (!c.has_csd && lmb) {
        if      (in_circle(mp, close_pos, 7.0f)) kill_focused();
        else if (in_circle(mp, max_pos,   7.0f)) toggle_maximize(&c);
        else if (in_circle(mp, min_pos,   7.0f)) toggle_minimize(&c);
    }

    // ── Double-click titlebar to maximize / restore ───────────
    if (!c.has_csd && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
        && in_rect(mp, tb_min, tb_max))
    {
        toggle_maximize(&c);
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_client_shadow(const Client& c)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 pos (c.x + 6,        c.y + 6);
    ImVec2 size(c.x + c.w + 12, c.y + c.h + 12);
    dl->AddRectFilled(pos, size, IM_COL32(0, 0, 0, 80), 12.0f);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_taskbar()
{
    const float BAR_H = 28.0f;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)m_sw, BAR_H});
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration          |
        ImGuiWindowFlags_NoMove                |
        ImGuiWindowFlags_NoScrollWithMouse     |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4, 2));
    ImGui::Begin("##taskbar", nullptr, flags);

    // Workspace buttons
    for (int i = 0; i < (int)m_workspaces.size(); ++i) {
        bool active = (i == m_current_ws);
        ImGui::PushStyleColor(ImGuiCol_Button,
            active ? ImVec4(0.25f, 0.50f, 0.80f, 1.0f)
                   : ImVec4(0.15f, 0.15f, 0.22f, 1.0f));
        std::string label = m_workspaces[i].name;
        if (!m_workspaces[i].clients.empty() && !active) label += " \xE2\x80\xA2";
        if (ImGui::Button(label.c_str(), {28, 20}))
            switch_workspace(i);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    ImGui::SameLine(0, 10);
    ImGui::Separator();
    ImGui::SameLine(0, 10);

    // Window list — ws.clients holds Window IDs; look up Client* via find_client
    for (Window cw : current_ws().clients) {
        Client* c = find_client(cw);
        if (!c) continue;

        ImVec4 col = c->minimized   ? ImVec4(0.10f, 0.10f, 0.15f, 0.8f)
                   : c == m_focused ? ImVec4(0.20f, 0.40f, 0.65f, 1.0f)
                                    : ImVec4(0.18f, 0.18f, 0.26f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        std::string label = c->title.substr(0, 20);
        if (c->minimized) label = "[" + label + "]";
        if (ImGui::Button(label.c_str(), {120, 20})) {
            if (c->minimized) toggle_minimize(c);
            else { focus(c); raise(c); }
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    // Right side — layout icon + cycle button
    const char* layout_icon =
        (current_ws().layout == Layout::Tiling)  ? "|=" :
        (current_ws().layout == Layout::Monocle) ? "[]" :
        (current_ws().layout == Layout::Ribbon)  ? "="  :
        (current_ws().layout == Layout::Split)   ? "||" :
                                                   "F";
    ImGui::SameLine((float)m_sw - 150.0f);
    if (ImGui::Button(layout_icon, {30, 20})) {
        auto& lay = current_ws().layout;
        if      (lay == Layout::Floating) lay = Layout::Tiling;
        else if (lay == Layout::Tiling)   lay = Layout::Monocle;
        else                              lay = Layout::Floating;
        apply_layout(current_ws());
    }

    ImGui::SameLine();
    if (ImGui::Button("[+]", {30, 20}))
        m_show_launcher = !m_show_launcher;

    ImGui::SameLine();
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    char tbuf[16];
    strftime(tbuf, sizeof(tbuf), "%H:%M", tm_info);
    ImGui::TextUnformatted(tbuf);

    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_launcher()
{
    // Reap finished child processes each frame (non-blocking)
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}

    ImVec2 center = {(float)m_sw * 0.5f, (float)m_sh * 0.4f};
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({440, 0}, ImGuiCond_Always);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration    |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6, 4));
    ImGui::Begin("##launcher", nullptr, flags);

    // Build filtered suggestion list from history
    std::vector<const std::string*> suggestions;
    std::string query(m_launcher_buf);
    for (const auto& cmd : m_launcher_history) {
        if (query.empty() || cmd.find(query) != std::string::npos)
            suggestions.push_back(&cmd);
    }
    std::reverse(suggestions.begin(), suggestions.end()); // most-recent first

    // Keyboard navigation state (static — lives across frames)
    static int  s_selected   = -1;
    static bool s_focus_input = true;
    static std::string s_last_query;

    if (query != s_last_query) {
        s_selected   = -1;
        s_last_query = query;
    }

    if (!suggestions.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            s_selected = std::min(s_selected + 1, (int)suggestions.size() - 1);
            s_focus_input = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            if (--s_selected < 0) { s_selected = -1; s_focus_input = true; }
        }
        if (s_selected >= 0 && s_selected < (int)suggestions.size()) {
            strncpy(m_launcher_buf, suggestions[s_selected]->c_str(),
                    sizeof(m_launcher_buf) - 1);
            m_launcher_buf[sizeof(m_launcher_buf) - 1] = '\0';
        }
    }

    if (s_focus_input) {
        ImGui::SetKeyboardFocusHere();
        s_focus_input = false;
    }

    ImGui::SetNextItemWidth(-1.0f);
    bool enter = ImGui::InputText("##cmd", m_launcher_buf, sizeof(m_launcher_buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);

    if (!suggestions.empty()) {
        ImGui::Separator();
        const int MAX_VISIBLE = 8;
        int show_n = std::min((int)suggestions.size(), MAX_VISIBLE);
        for (int i = 0; i < show_n; ++i) {
            bool selected = (i == s_selected);
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.40f, 0.65f, 1.0f));
            if (ImGui::Selectable(suggestions[i]->c_str(), selected)) {
                strncpy(m_launcher_buf, suggestions[i]->c_str(),
                        sizeof(m_launcher_buf) - 1);
                m_launcher_buf[sizeof(m_launcher_buf) - 1] = '\0';
                enter = true;
            }
            ImGui::PopStyleColor();
            if (selected) ImGui::SetScrollHereY(0.5f);
        }
    }

    if (enter && m_launcher_buf[0] != '\0') {
        const std::string cmd(m_launcher_buf);
        pid_t pid = fork();
        if (pid == 0) {
            // New session — detach from WM's process group entirely
            setsid();
            // Close the X connection fd so child doesn't hold it open
            if (m_dpy) close(ConnectionNumber(m_dpy));
            // Redirect stdin/stdout/stderr to /dev/null.
            // Without this the child inherits the WM's fds, so anything
            // the WM printf()s ends up in the terminal's pty as garbage output.
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
            execlp("sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(1);
        } else if (pid > 0) {
            printf("[launcher] Spawned \"%s\" pid=%d\n", cmd.c_str(), pid);
        } else {
            perror("[launcher] fork");
        }

        m_launcher_history.erase(
            std::remove(m_launcher_history.begin(),
                        m_launcher_history.end(), cmd),
            m_launcher_history.end());
        m_launcher_history.push_back(cmd);

        const size_t MAX_HISTORY = 50;
        if (m_launcher_history.size() > MAX_HISTORY)
            m_launcher_history.erase(m_launcher_history.begin(),
                m_launcher_history.begin() +
                    (m_launcher_history.size() - MAX_HISTORY));

        memset(m_launcher_buf, 0, sizeof(m_launcher_buf));
        s_selected    = -1;
        s_last_query  = "";
        s_focus_input = true;
        m_show_launcher = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        memset(m_launcher_buf, 0, sizeof(m_launcher_buf));
        s_selected    = -1;
        s_last_query  = "";
        s_focus_input = true;
        m_show_launcher = false;
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_notifications()
{
    const float DT = ImGui::GetIO().DeltaTime;
    float y = 40.0f;

    for (int i = (int)m_notifications.size() - 1; i >= 0; --i) {
        auto& n = m_notifications[i];
        n.ttl -= DT;
        if (n.ttl <= 0) {
            m_notifications.erase(m_notifications.begin() + i);
            continue;
        }

        float alpha = std::min(1.0f, n.ttl);
        ImGui::SetNextWindowPos({(float)m_sw - 310.0f, y});
        ImGui::SetNextWindowSize({300, 0});
        ImGui::SetNextWindowBgAlpha(alpha * 0.85f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration     |
            ImGuiWindowFlags_NoMove           |
            ImGuiWindowFlags_NoSavedSettings  |
            ImGuiWindowFlags_AlwaysAutoResize;

        std::string wid = "##notif_" + std::to_string(i);
        ImGui::Begin(wid.c_str(), nullptr, flags);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::TextWrapped("%s", n.text.c_str());
        ImGui::PopStyleVar();
        ImGui::End();

        y += 60.0f;
    }
}
