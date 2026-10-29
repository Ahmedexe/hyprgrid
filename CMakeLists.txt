cmake_minimum_required(VERSION 3.27)

project(hyprgrid-native
    DESCRIPTION "Native cancellable 2D workspace-grid gestures for Hyprland"
    VERSION 0.7.1
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hyprgrid-native SHARED src/main.cpp)

# Without this, GCC marks some symbols STB_GNU_UNIQUE, which makes the .so
# impossible to dlclose(), so `hyprctl plugin unload/load` keeps running old code.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(hyprgrid-native PRIVATE -fno-gnu-unique)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
)
target_link_libraries(hyprgrid-native PRIVATE rt PkgConfig::deps)

# Installs $out/lib/libhyprgrid-native.so
install(TARGETS hyprgrid-native)
