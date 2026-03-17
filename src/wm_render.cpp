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

    // Grab window pixels via XGetImage (works everywhere)
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

    // Create texture on first use
    if (!c.tex) {
        glGenTextures(1, &c.tex);
        glBindTexture(GL_TEXTURE_2D, c.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, c.tex);
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 c.w, c.h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
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
    // Iterate only over clients in the current workspace
    for (auto* c : current_ws().clients) {
        if (c->minimized) continue;

        // Update texture if needed
        if (c->dirty) {
            composite_update_texture(*c);
            c->dirty = false;
        }

        // Render the client window
        render_client_window(*c);
    }
}


// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_client_window(Client& c)
{
    const float TITLEBAR_H = 22.0f;

    ImVec2 win_pos  = {(float)c.x, (float)c.y - TITLEBAR_H};
    ImVec2 win_size = {(float)c.w, (float)c.h + TITLEBAR_H};

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

    // Traffic-light buttons
    ImVec2 close_pos = {tl.x + win_size.x - 16.0f, tl.y + 11.0f};
    ImVec2 max_pos   = {tl.x + win_size.x - 36.0f, tl.y + 11.0f};
    ImVec2 min_pos   = {tl.x + win_size.x - 56.0f, tl.y + 11.0f};

    dl->AddCircleFilled(close_pos, 7.0f, IM_COL32(220,  70,  70, 255));
    dl->AddCircleFilled(max_pos,   7.0f, IM_COL32( 70, 180,  70, 255));
    dl->AddCircleFilled(min_pos,   7.0f, IM_COL32(220, 180,  50, 255));

    // Window content
    ImVec2 content_size = {(float)c.w, (float)c.h};
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

    // Focus / drag on click
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        if (ImGui::IsMouseClicked(0)) {
            focus(&c);
            raise(&c);
        }
    }

    // Traffic-light click detection
    ImVec2 mp = ImGui::GetMousePos();
    auto in_circle = [](ImVec2 p, ImVec2 centre, float r) {
        float dx = p.x - centre.x, dy = p.y - centre.y;
        return dx*dx + dy*dy <= r*r;
    };
    if (ImGui::IsMouseClicked(0)) {
        if      (in_circle(mp, close_pos, 7.0f)) kill_focused();
        else if (in_circle(mp, max_pos,   7.0f)) toggle_maximize(&c);
        else if (in_circle(mp, min_pos,   7.0f)) toggle_minimize(&c);
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_taskbar()
{
    const float BAR_H = 28.0f;

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)m_sw, BAR_H});
    ImGui::SetNextWindowBgAlpha(0.88f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration         |
        ImGuiWindowFlags_NoMove               |
        ImGuiWindowFlags_NoScrollWithMouse    |
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

    // Window list
    for (auto* c : current_ws().clients) {
        ImVec4 col = c->minimized     ? ImVec4(0.10f, 0.10f, 0.15f, 0.8f)
                   : c == m_focused   ? ImVec4(0.20f, 0.40f, 0.65f, 1.0f)
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

    // Right side
    const char* layout_icon =
        (current_ws().layout == Layout::Tiling)  ? "[T]" :
        (current_ws().layout == Layout::Monocle) ? "[M]" : "[F]";

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
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm_info);
    ImGui::TextUnformatted(tbuf);

    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_launcher()
{
    ImVec2 center = {(float)m_sw * 0.5f, (float)m_sh * 0.4f};
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({400, 60}, ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration  |
        ImGuiWindowFlags_NoMove        |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::Begin("##launcher", nullptr, flags);

    ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##cmd", m_launcher_buf, sizeof(m_launcher_buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);

    if (enter && m_launcher_buf[0] != '\0') {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execlp("sh", "sh", "-c", m_launcher_buf, (char*)nullptr);
            _exit(1);
        }
        memset(m_launcher_buf, 0, sizeof(m_launcher_buf));
        m_show_launcher = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        m_show_launcher = false;

    ImGui::End();
    ImGui::PopStyleVar();
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
