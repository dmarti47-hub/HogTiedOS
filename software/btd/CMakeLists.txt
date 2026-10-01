cmake_minimum_required(VERSION 3.16)
project(hbas-btd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

set(HBAS_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../libhbas libhbas)

add_executable(hbas-btd src/btd.c)
target_include_directories(hbas-btd PRIVATE ${DBUS_INCLUDE_DIRS})
target_link_libraries(hbas-btd PRIVATE hbas ${DBUS_LINK_LIBRARIES})
target_compile_options(hbas-btd PRIVATE -Wall -Wextra -Werror)
install(TARGETS hbas-btd RUNTIME DESTINATION bin)
