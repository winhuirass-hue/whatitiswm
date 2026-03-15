/**
 * ImGui X11 Window Manager
 * A tiling/floating WM using Dear ImGui for its compositor/overlay UI.
 *
 * Build: see Makefile
 * Deps:  libx11, libxcomposite, libxdamage, libxrender, libgl, libglx,
 *        imgui (vendored), glfw3 or raw GLX
 */

#include "wm.hpp"
#include <cstdio>
#include <cstdlib>
#include <csignal>

static ImGuiWM* g_wm = nullptr;

static void handle_signal(int sig) {
    if (g_wm) g_wm->request_quit();
}

int main(int argc, char** argv) {
    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);

    // Must be the first thing: claim display :0 (or $DISPLAY)
    const char* display_name = getenv("DISPLAY");
    if (!display_name) display_name = ":0";

    ImGuiWM wm(display_name);
    g_wm = &wm;

    if (!wm.init()) {
        fprintf(stderr, "[imgui-wm] Failed to initialize\n");
        return 1;
    }

    wm.run();          // blocks until quit

    wm.shutdown();
    return 0;
}
