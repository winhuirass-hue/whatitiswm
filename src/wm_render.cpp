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

// GLX_EXT_texture_from_pixmap constants (may not be in older headers)
#ifndef GLX_TEXTURE_2D_EXT
#define GLX_TEXTURE_2D_EXT          0x20DC
#define GLX_FRONT_LEFT_EXT          0x20DE
#define GLX_TEXTURE_FORMAT_EXT      0x20D5
#define GLX_TEXTURE_FORMAT_RGBA_EXT 0x20D3
#define GLX_TEXTURE_FORMAT_RGB_EXT  0x20D2
#define GLX_BIND_TO_TEXTURE_RGBA_EXT 0x20D1
#define GLX_BIND_TO_TEXTURE_RGB_EXT  0x20D0
#endif

typedef void (*PFNGLXBINDTEXIMAGEEXTPROC)(Display*, GLXDrawable, int, const int*);
typedef void (*PFNGLXRELEASETEXIMAGEEXTPROC)(Display*, GLXDrawable, int);

static PFNGLXBINDTEXIMAGEEXTPROC    glXBindTexImageEXT    = nullptr;
static PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT = nullptr;

// ─────────────────────────────────────────────────────────────
void ImGuiWM::composite_bind_window(Client& c)
{
    // Load extension procs once
    if (!glXBindTexImageEXT) {
        glXBindTexImageEXT    = (PFNGLXBINDTEXIMAGEEXTPROC)
            glXGetProcAddress((const GLubyte*)"glXBindTexImageEXT");
        glXReleaseTexImageEXT = (PFNGLXRELEASETEXIMAGEEXTPROC)
            glXGetProcAddress((const GLubyte*)"glXReleaseTexImageEXT");
    }

    if (!glXBindTexImageEXT) {
        // Fallback: allocate a plain GL texture; we'll blit via XGetImage
        glGenTextures(1, &c.tex);
        glBindTexture(GL_TEXTURE_2D, c.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    // GLX_EXT_texture_from_pixmap path
    c.pixmap = XCompositeNameWindowPixmap(m_dpy, c.xwin);

    static int pixmap_config[] = {
        GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGBA_EXT,
        None
    };

    // Pick an RGBA FBConfig
    int n = 0;
    GLXFBConfig* fbs = glXGetFBConfigs(m_dpy, m_screen, &n);
    GLXFBConfig  chosen = 0;
    for (int i = 0; i < n && !chosen; ++i) {
        int val = 0;
        glXGetFBConfigAttrib(m_dpy, fbs[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &val);
        if (val) chosen = fbs[i];
    }
    if (fbs) XFree(fbs);

    if (!chosen) {
        // fallback
        glGenTextures(1, &c.tex);
        return;
    }

    c.glx_pix = glXCreatePixmap(m_dpy, chosen, c.pixmap, pixmap_config);

    glGenTextures(1, &c.tex);
    glBindTexture(GL_TEXTURE_2D, c.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glXBindTexImageEXT(m_dpy, c.glx_pix, GLX_FRONT_LEFT_EXT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    c.dirty = false;
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::composite_release_window(Client& c)
{
    if (c.tex) {
        if (c.glx_pix && glXReleaseTexImageEXT)
            glXReleaseTexImageEXT(m_dpy, c.glx_pix, GLX_FRONT_LEFT_EXT);
        glDeleteTextures(1, &c.tex);
        c.tex = 0;
    }
    if (c.glx_pix) {
        glXDestroyPixmap(m_dpy, c.glx_pix);
        c.glx_pix = 0;
    }
    if (c.pixmap) {
        XFreePixmap(m_dpy, c.pixmap);
        c.pixmap = 0;
    }
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

    if (!c.tex) return;

    // If GLX_EXT_texture_from_pixmap is available, rebind
    if (c.glx_pix && glXBindTexImageEXT && glXReleaseTexImageEXT) {
        glBindTexture(GL_TEXTURE_2D, c.tex);
        glXReleaseTexImageEXT(m_dpy, c.glx_pix, GLX_FRONT_LEFT_EXT);
        // Re-create pixmap from current window contents
        if (c.pixmap) XFreePixmap(m_dpy, c.pixmap);
        c.pixmap = XCompositeNameWindowPixmap(m_dpy, c.xwin);
        glXBindTexImageEXT(m_dpy, c.glx_pix, GLX_FRONT_LEFT_EXT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    // Fallback: XGetImage → glTexImage2D (slow but portable)
    XImage* img = XGetImage(m_dpy, c.xwin, 0, 0, c.w, c.h, AllPlanes, ZPixmap);
    if (!img) return;

    // Convert BGR(A) → RGBA
    std::vector<unsigned char> buf(c.w * c.h * 4);
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            unsigned long px = XGetPixel(img, x, y);
            int off = (y * c.w + x) * 4;
            buf[off + 0] = (px >> 16) & 0xff;
            buf[off + 1] = (px >>  8) & 0xff;
            buf[off + 2] = (px >>  0) & 0xff;
            buf[off + 3] = 255;
        }
    }
    XDestroyImage(img);

    glBindTexture(GL_TEXTURE_2D, c.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c.w, c.h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ─────────────────────────────────────────────────────────────
// Main render loop
// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_frame()
{
    // Update dirty composite textures
    for (auto& c : m_clients)
        if (c.dirty) composite_update_texture(c);

    // New ImGui frame
    ImGuiIO& io = ImGui::GetIO();
    auto now = Clock::now();
    io.DeltaTime = std::chrono::duration<float>(now - m_last_frame).count();
    ImGui::NewFrame();

    render_desktop();
    render_clients();
    render_taskbar();

    if (m_show_launcher) render_launcher();

    render_notifications();

    // Render
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
    // Wallpaper-style background gradient (ImGui draw-list)
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
    // Draw X window textures as ImGui images
    for (auto& c : m_clients) {
        if (c.minimized) continue;

        // Skip windows not on current workspace
        bool on_ws = false;
        for (auto* wc : current_ws().clients)
            if (wc == &c) { on_ws = true; break; }
        if (!on_ws) continue;

        render_client_window(c);
    }
}

// ─────────────────────────────────────────────────────────────
void ImGuiWM::render_client_window(Client& c)
{
    const float TITLEBAR_H = 22.0f;

    // Position and size
    ImVec2 win_pos  = {(float)c.x, (float)c.y - TITLEBAR_H};
    ImVec2 win_size = {(float)c.w, (float)c.h + TITLEBAR_H};

    ImGui::SetNextWindowPos(win_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(win_size, ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize         |
        ImGuiWindowFlags_NoScrollbar      |
        ImGuiWindowFlags_NoScrollWithMouse|
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    // Focused highlight
    if (c.focused) {
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.2f, 0.45f, 0.75f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.15f, 0.15f, 0.20f, 1.0f));
    }

    std::string wid = "##win_" + std::to_string(c.xwin);
    ImGui::Begin((c.title + wid).c_str(), nullptr, flags);

    // Title bar buttons (drawn in the title bar area via draw list)
    ImVec2 tl = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Close button
    ImVec2 close_pos = {tl.x + win_size.x - 16.0f, tl.y + 4.0f};
    dl->AddCircleFilled(close_pos, 7.0f, IM_COL32(220, 70, 70, 255));

    // Maximise button
    ImVec2 max_pos = {tl.x + win_size.x - 36.0f, tl.y + 4.0f};
    dl->AddCircleFilled(max_pos, 7.0f, IM_COL32(70, 180, 70, 255));

    // Minimise button
    ImVec2 min_pos = {tl.x + win_size.x - 56.0f, tl.y + 4.0f};
    dl->AddCircleFilled(min_pos, 7.0f, IM_COL32(220, 180, 50, 255));

    // Composite content area
    ImVec2 content_pos  = ImGui::GetCursorScreenPos();
    ImVec2 content_size = {(float)c.w, (float)c.h};

    if (c.tex) {
        ImGui::Image((ImTextureID)(intptr_t)c.tex,
                     content_size,
                     ImVec2(0, 0), ImVec2(1, 1));
    } else {
        // Placeholder if texture not ready
        dl->AddRectFilled(content_pos,
            {content_pos.x + content_size.x, content_pos.y + content_size.y},
            IM_COL32(30, 30, 40, 255));
        ImGui::Dummy(content_size);
    }

    // Handle title-bar click → focus / drag
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsMouseClicked(0)) {
            focus(&c);
            raise(&c);
        }
    }

    // Detect button clicks via mouse position
    ImVec2 mp = ImGui::GetMousePos();
    auto in_circle = [](ImVec2 p, ImVec2 c2, float r) {
        float dx = p.x - c2.x, dy = p.y - c2.y;
        return dx*dx + dy*dy <= r*r;
    };

    if (ImGui::IsMouseClicked(0)) {
        if (in_circle(mp, close_pos, 7.0f))
            kill_focused();
        else if (in_circle(mp, max_pos, 7.0f))
            toggle_maximize(&c);
        else if (in_circle(mp, min_pos, 7.0f))
            toggle_minimize(&c);
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
        ImGuiWindowFlags_NoDecoration        |
        ImGuiWindowFlags_NoMove              |
        ImGuiWindowFlags_NoScrollWithMouse   |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4, 2));
    ImGui::Begin("##taskbar", nullptr, flags);

    // ── Workspace buttons ────────────────────────────────────
    for (int i = 0; i < (int)m_workspaces.size(); ++i) {
        bool active = (i == m_current_ws);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.80f, 1.0f));
        else
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.22f, 1.0f));

        std::string label = m_workspaces[i].name;
        // Indicator dot if workspace has windows
        if (!m_workspaces[i].clients.empty() && !active)
            label += " ●";

        if (ImGui::Button(label.c_str(), {28, 20}))
            switch_workspace(i);

        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    ImGui::SameLine(0, 10);
    ImGui::Separator();
    ImGui::SameLine(0, 10);

    // ── Window list (current workspace) ─────────────────────
    for (auto* c : current_ws().clients) {
        if (c->minimized) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.15f, 0.8f));
        } else if (c == m_focused) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.65f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.26f, 1.0f));
        }

        std::string label = c->title.substr(0, 20);
        if (c->minimized) label = "[" + label + "]";

        if (ImGui::Button(label.c_str(), {120, 20})) {
            if (c->minimized) toggle_minimize(c);
            else { focus(c); raise(c); }
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    // ── Right side: layout indicator + clock ────────────────
    const char* layout_icon =
        (current_ws().layout == Layout::Tiling)  ? "[T]" :
        (current_ws().layout == Layout::Monocle) ? "[M]" : "[F]";

    float clock_w = 80.0f;
    float layout_w = 35.0f;
    ImGui::SameLine((float)m_sw - clock_w - layout_w - 40.0f);

    if (ImGui::Button(layout_icon, {layout_w, 20})) {
        // Cycle layout
        auto& lay = current_ws().layout;
        if      (lay == Layout::Floating) lay = Layout::Tiling;
        else if (lay == Layout::Tiling)   lay = Layout::Monocle;
        else                              lay = Layout::Floating;
        apply_layout(current_ws());
    }

    ImGui::SameLine();

    // Launcher button
    if (ImGui::Button("[+]", {30, 20}))
        m_show_launcher = !m_show_launcher;

    ImGui::SameLine();

    // Clock
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char tbuf[16];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
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
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::Begin("##launcher", nullptr, flags);

    ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##cmd", m_launcher_buf, sizeof(m_launcher_buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);

    if (enter && m_launcher_buf[0] != '\0') {
        // Fork/exec
        if (fork() == 0) {
            setsid();
            execlp("sh", "sh", "-c", m_launcher_buf, nullptr);
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
            ImGuiWindowFlags_NoDecoration    |
            ImGuiWindowFlags_NoMove          |
            ImGuiWindowFlags_NoSavedSettings |
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
