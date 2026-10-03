cmake_minimum_required(VERSION 3.16)
project(hbas-tunerd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

set(HBAS_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../libhbas libhbas)

add_executable(hbas-tunerd src/tunerd.c)
target_link_libraries(hbas-tunerd PRIVATE hbas)
target_compile_options(hbas-tunerd PRIVATE -Wall -Wextra -Werror)
install(TARGETS hbas-tunerd RUNTIME DESTINATION bin)
