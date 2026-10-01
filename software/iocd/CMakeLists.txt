cmake_minimum_required(VERSION 3.16)
project(hbas-iocd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

set(HBAS_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../libhbas libhbas)

add_executable(hbas-iocd src/iocd.c)
target_link_libraries(hbas-iocd PRIVATE hbas)
target_compile_options(hbas-iocd PRIVATE -Wall -Wextra -Werror)
install(TARGETS hbas-iocd RUNTIME DESTINATION bin)
