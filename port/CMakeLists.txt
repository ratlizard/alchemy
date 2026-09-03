cmake_minimum_required(VERSION 3.20)
project(cythera_port CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# The emulated guest is big-endian 32-bit; every guest access goes through the
# byte-swapping accessors in mem.h, so the host stays plain little-endian ARM64.
# SDL2 provides the window, input and (later) audio. Homebrew ships a CMake
# config; fall back to pkg-config where it does not.
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2)
endif()

add_library(cyt_core STATIC
  src/mem.cpp
  src/pef.cpp
  src/ppc.cpp
  src/toolbox.cpp
  src/mac/heap.cpp
  src/mac/memory_mgr.cpp
  src/resfork.cpp
  src/mac/resource_mgr.cpp
  src/mac/core.cpp
  src/mac/menu_mgr.cpp
  src/mac/file_mgr.cpp
  src/mac/file_calls.cpp
  src/mac/standard_file.cpp
  src/mac/quickdraw.cpp
  src/mac/thread_mgr.cpp
  src/mac/event_mgr.cpp
  src/mac/apple_event.cpp
  src/mac/quicktime.cpp
  src/mac/qd_surface.cpp
  src/mac/qd_region.cpp
  src/mac/pict.cpp
  src/mac/qd_draw.cpp
  src/mac/window_mgr.cpp
  src/mac/dialog_mgr.cpp
  src/mac/control_mgr.cpp
  src/mac/list_mgr.cpp
  src/mac/video_driver.cpp
  src/mac/font_mgr.cpp
  src/host/display.cpp
  src/host/hostfont.cpp
)
target_include_directories(cyt_core PUBLIC src)
# CoreText rasterises the two faces that cannot come from the game's bitmaps:
# its own scalable display face, and the System-file fonts it names by name.
target_link_libraries(cyt_core PUBLIC SDL2::SDL2
  "-framework CoreText" "-framework CoreGraphics" "-framework CoreFoundation")

add_executable(cythera src/main.cpp)
target_link_libraries(cythera PRIVATE cyt_core)

# Text rendering cannot be demonstrated by running the game yet -- it stops at
# its splash screen, before it draws a character -- so this resolves every one
# of Cythera's twenty 'TxSt' style resources and rasterises the strike behind
# it. That is the game's complete text vocabulary, checked directly.
add_executable(cyt_font_test tests/font_test.cpp)
target_link_libraries(cyt_font_test PRIVATE cyt_core)

# The Resource Manager can now write, and a resource fork is a format with two
# "count minus one" fields and offsets measured from three different origins.
# This round-trips the writer through the reader, which is what stands between
# a saved game and a corrupted one.
add_executable(cyt_resfork_test tests/resfork_test.cpp)
target_link_libraries(cyt_resfork_test PRIVATE cyt_core)
