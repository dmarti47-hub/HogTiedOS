cmake_minimum_required(VERSION 3.16)
project(libhbas C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)          # tests use statement expressions

add_library(hbas STATIC src/vehicle.c)
target_include_directories(hbas PUBLIC include)
target_compile_options(hbas PRIVATE -Wall -Wextra -Werror)

option(HBAS_TESTS "Build libhbas unit tests" ON)
if(HBAS_TESTS)
  enable_testing()
  add_executable(test_vehicle tests/test_vehicle.c)
  target_link_libraries(test_vehicle hbas)
  target_compile_options(test_vehicle PRIVATE -Wall -Wextra -Werror)
  add_test(NAME vehicle COMMAND test_vehicle)
endif()

install(TARGETS hbas ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
