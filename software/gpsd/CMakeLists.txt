cmake_minimum_required(VERSION 3.16)
project(hbas-gpsd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

set(HBAS_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../libhbas libhbas)

add_executable(hbas-gpsd src/gpsd.c)
target_link_libraries(hbas-gpsd PRIVATE hbas)
target_compile_options(hbas-gpsd PRIVATE -Wall -Wextra -Werror)
install(TARGETS hbas-gpsd RUNTIME DESTINATION bin)
