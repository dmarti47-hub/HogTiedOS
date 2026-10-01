cmake_minimum_required(VERSION 3.16)
project(hbas-map C CXX)

# libosmscout (core, map, AGG painter); point CMAKE_PREFIX_PATH at its install
find_package(libosmscout REQUIRED)
find_package(Freetype REQUIRED)        # the AGG painter's headers include FreeType's
find_package(PkgConfig REQUIRED)
pkg_check_modules(AGG REQUIRED libagg)

add_library(hbasmap STATIC src/map.cpp)
target_include_directories(hbasmap PUBLIC include)
target_compile_features(hbasmap PRIVATE cxx_std_20)
target_include_directories(hbasmap PRIVATE ${AGG_INCLUDE_DIRS})
target_link_libraries(hbasmap PUBLIC OSMScout::OSMScout OSMScout::Map OSMScout::MapAGG
                      Freetype::Freetype ${AGG_LINK_LIBRARIES})
target_compile_options(hbasmap PRIVATE -Wall -Wextra)

# map-render-test: draw one frame to a .ppm, and time it
add_executable(map-render-test tools/map-render-test.c)
set_target_properties(map-render-test PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(map-render-test PRIVATE hbasmap)

# map-route-test: route between two points in every mode, and time it
add_executable(map-route-test tools/map-route-test.c)
set_target_properties(map-route-test PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(map-route-test PRIVATE hbasmap m)
