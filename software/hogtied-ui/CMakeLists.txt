cmake_minimum_required(VERSION 3.16)
project(hogtied-ui C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

option(HOGTIED_FBDEV "Build the Linux framebuffer backend (target)" OFF)
option(HOGTIED_SDL "Build the desktop window backend (PC, needs libsdl2-dev)" OFF)
option(HOGTIED_MAP "Build the map page (needs libosmscout; software/hbas-map)" OFF)
if(HOGTIED_FBDEV AND HOGTIED_SDL)
  message(FATAL_ERROR "Pick one backend: HOGTIED_FBDEV or HOGTIED_SDL")
endif()
set(LVGL_DIR "" CACHE PATH "Path to an LVGL v9.2.2 source tree")

if(NOT LVGL_DIR)
  message(FATAL_ERROR "Set -DLVGL_DIR=<lvgl v9.2.2 source> "
                      "(https://github.com/lvgl/lvgl/archive/refs/tags/v9.2.2.tar.gz)")
endif()

# LVGL picks up our lv_conf.h through LV_CONF_PATH
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
# Always link LVGL statically: Buildroot passes BUILD_SHARED_LIBS=ON, which
# built a liblvgl.so.9 that EXCLUDE_FROM_ALL never installs, so the app
# couldn't start on the target.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${LVGL_DIR} lvgl EXCLUDE_FROM_ALL)
if(HOGTIED_FBDEV)
  target_compile_definitions(lvgl PUBLIC HOGTIED_FBDEV)
endif()
if(HOGTIED_SDL)
  if(NOT SDL2_LIBRARIES)            # normally from pkg-config; can be preset
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    # full paths: plain SDL2_LIBRARIES is just "SDL2" without its -L dir
    set(SDL2_LIBRARIES ${SDL2_LINK_LIBRARIES})
  endif()
  target_compile_definitions(lvgl PUBLIC HOGTIED_SDL)
  target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})
  target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
endif()

set(HBAS_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../libhbas libhbas)

add_executable(hogtied-ui src/main.c src/ui.c src/ui_audio.c src/ui_eq.c src/ui_media.c src/ui_gps.c
  src/ui_map.c src/ui_kbd.c src/ui_widgets.c src/demo.c src/persist.c)
target_link_libraries(hogtied-ui PRIVATE lvgl hbas m)
if(HOGTIED_MAP)
  enable_language(CXX)
  add_subdirectory(../hbas-map hbas-map)
  find_package(Threads REQUIRED)
  target_link_libraries(hogtied-ui PRIVATE hbasmap Threads::Threads)
  target_compile_definitions(hogtied-ui PRIVATE HOGTIED_MAP)
  set_target_properties(hogtied-ui PROPERTIES LINKER_LANGUAGE CXX)
endif()
target_compile_options(hogtied-ui PRIVATE -Wall -Wextra -Werror)
install(TARGETS hogtied-ui RUNTIME DESTINATION bin)
