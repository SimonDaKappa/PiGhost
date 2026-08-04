cmake_minimum_required(VERSION 3.20)
project(sine_wave_cpu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

add_executable(sine_wave_cpu main.c)
target_include_directories(sine_wave_cpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../libpgdp)

find_package(Threads REQUIRED)
target_link_libraries(sine_wave_cpu PRIVATE Threads::Threads m rt)
