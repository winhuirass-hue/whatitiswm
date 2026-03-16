CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra \
             -Iinclude \
             -Ithird_party/imgui \
             -Ithird_party/imgui/backends \
             $(shell pkg-config --cflags x11 xcomposite xdamage xrender xfixes gl)

LDFLAGS := $(shell pkg-config --libs x11 xcomposite xdamage xrender xfixes gl) \
            -lX11 -lXext -lXcomposite -lXdamage -lXrender -lXfixes \
            -lGL -lGLX -ldl

IMGUI_DIR  := third_party/imgui
IMGUI_SRCS := $(IMGUI_DIR)/imgui.cpp \
              $(IMGUI_DIR)/imgui_draw.cpp \
              $(IMGUI_DIR)/imgui_tables.cpp \
              $(IMGUI_DIR)/imgui_widgets.cpp \
              $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

WM_SRCS    := src/main.cpp \
              src/wm_init.cpp \
              src/wm_events.cpp \
              src/wm_clients.cpp \
              src/wm_render.cpp

SRCS := $(WM_SRCS) $(IMGUI_SRCS)
OBJS := $(SRCS:.cpp=.o)

TARGET := imgui-wm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

# ─── Install Dear ImGui ───────────────────────────────────────
# Clones imgui into third_party/ if not present.
imgui:
	@mkdir -p third_party
	git clone --depth 1 --branch v1.90.4 \
	    https://github.com/ocornut/imgui.git third_party/imgui

.PHONY: all clean imgui
