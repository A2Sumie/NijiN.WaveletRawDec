# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
project(nijinWVDC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(nijinWVDC_core STATIC
    src/nev_container.cpp
    src/arw_container.cpp
    src/ticoraw_dec.cpp
    src/arw6_dec.cpp
    src/dng_writer.cpp
)
target_include_directories(nijinWVDC_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(nijinWVDC_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nijinWVDC_core PUBLIC Threads::Threads)

add_executable(nijinWVDC src/main.cpp)
target_compile_options(nijinWVDC PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nijinWVDC PRIVATE nijinWVDC_core)

enable_testing()
add_executable(test_nijinWVDC tests/test_nijinWVDC.cpp)
target_compile_options(test_nijinWVDC PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_nijinWVDC PRIVATE nijinWVDC_core)
add_test(NAME test_nijinWVDC COMMAND test_nijinWVDC)
