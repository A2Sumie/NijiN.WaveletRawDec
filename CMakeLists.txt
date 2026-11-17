# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
project(nev2bayer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(nev2bayer_core STATIC
    src/nev_container.cpp
    src/ticoraw_dec.cpp
    src/dng_writer.cpp
)
target_include_directories(nev2bayer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(nev2bayer_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nev2bayer_core PUBLIC Threads::Threads)

add_executable(nev2bayer src/main.cpp)
target_compile_options(nev2bayer PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nev2bayer PRIVATE nev2bayer_core)

enable_testing()
add_executable(test_nev2bayer tests/test_nev2bayer.cpp)
target_compile_options(test_nev2bayer PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_nev2bayer PRIVATE nev2bayer_core)
add_test(NAME test_nev2bayer COMMAND test_nev2bayer)
