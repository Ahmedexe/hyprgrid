# Builds against the Hyprland headers found by pkg-config
# (on Arch they come with the `hyprland` package).

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++23 -Wall -Wextra -Wpedantic
PKG_CONFIG ?= pkg-config

# GCC only: keep the .so unloadable so `hyprctl plugin unload/load` picks up rebuilds.
ifeq (,$(findstring clang,$(shell $(CXX) --version 2>/dev/null)))
    EXTRA_FLAGS = -fno-gnu-unique
endif

# Hyprland's headers are treated as system headers so their warnings stay quiet.
DEP_CFLAGS = $(patsubst -I%,-isystem %,$(shell $(PKG_CONFIG) --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon))

.PHONY: all clean

all: hyprgrid-native.so

hyprgrid-native.so: src/main.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) $< -o $@ $(DEP_CFLAGS)

clean:
	rm -f hyprgrid-native.so
