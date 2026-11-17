# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
project(nijinWVDC VERSION 0.2.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(NIJINWVDC_BUILD_SHARED "Build nijinWVDC shared library" ON)
option(NIJINWVDC_BUILD_CLI "Build nijinWVDC CLI executable" ON)
option(NIJINWVDC_BUILD_TESTS "Build C++ and C verification suites" ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

find_package(Threads REQUIRED)

set(NIJINWVDC_LIB_SOURCES
    src/nev_container.cpp
    src/arw_container.cpp
    src/ticoraw_dec.cpp
    src/arw6_dec.cpp
    src/dng_writer.cpp
    src/session_api.cpp
    src/c_api.cpp
)

function(nijinwvdc_apply_target_options tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /WX /EHsc /MP)
        target_compile_definitions(${tgt} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(nijinWVDC_static STATIC ${NIJINWVDC_LIB_SOURCES})
add_library(nijinWVDC::static ALIAS nijinWVDC_static)
if(MSVC)
    set_target_properties(nijinWVDC_static PROPERTIES OUTPUT_NAME "nijinWVDC_static" EXPORT_NAME "static")
else()
    set_target_properties(nijinWVDC_static PROPERTIES OUTPUT_NAME "nijinWVDC" EXPORT_NAME "static")
endif()
target_compile_features(nijinWVDC_static PUBLIC cxx_std_17)
target_compile_definitions(nijinWVDC_static PUBLIC NIJINWVDC_STATIC)
target_include_directories(nijinWVDC_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(nijinWVDC_static PUBLIC Threads::Threads)
nijinwvdc_apply_target_options(nijinWVDC_static)
if(UNIX AND NOT APPLE)
    target_link_libraries(nijinWVDC_static PUBLIC m)
endif()

set(NIJINWVDC_INSTALL_TARGETS nijinWVDC_static)

if(NIJINWVDC_BUILD_SHARED)
    add_library(nijinWVDC_shared SHARED ${NIJINWVDC_LIB_SOURCES})
    add_library(nijinWVDC::shared ALIAS nijinWVDC_shared)
    set_target_properties(nijinWVDC_shared PROPERTIES
        OUTPUT_NAME "nijinWVDC"
        EXPORT_NAME "shared"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    if(APPLE)
        set_target_properties(nijinWVDC_shared PROPERTIES
            MACOSX_RPATH ON
            INSTALL_NAME_DIR "@rpath"
            BUILD_RPATH "@loader_path"
            INSTALL_RPATH "@loader_path"
        )
    elseif(UNIX)
        set_target_properties(nijinWVDC_shared PROPERTIES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN"
        )
    endif()
    target_compile_features(nijinWVDC_shared PUBLIC cxx_std_17)
    target_compile_definitions(nijinWVDC_shared PRIVATE NIJINWVDC_BUILDING_DLL)
    target_include_directories(nijinWVDC_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(nijinWVDC_shared PRIVATE Threads::Threads)
    nijinwvdc_apply_target_options(nijinWVDC_shared)
    if(UNIX AND NOT APPLE)
        target_link_libraries(nijinWVDC_shared PRIVATE m)
    endif()
    list(APPEND NIJINWVDC_INSTALL_TARGETS nijinWVDC_shared)
endif()

if(NIJINWVDC_BUILD_CLI)
    add_executable(nijinWVDC src/main.cpp)
    target_compile_features(nijinWVDC PRIVATE cxx_std_17)
    target_link_libraries(nijinWVDC PRIVATE nijinWVDC_static)
    nijinwvdc_apply_target_options(nijinWVDC)
    if(APPLE)
        set_target_properties(nijinWVDC PROPERTIES INSTALL_RPATH "@executable_path/../${CMAKE_INSTALL_LIBDIR}")
    elseif(UNIX)
        set_target_properties(nijinWVDC PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
    list(APPEND NIJINWVDC_INSTALL_TARGETS nijinWVDC)
endif()

if(NIJINWVDC_BUILD_TESTS)
    enable_testing()
    add_executable(test_nijinWVDC tests/test_nijinWVDC.cpp)
    target_compile_features(test_nijinWVDC PRIVATE cxx_std_17)
    target_link_libraries(test_nijinWVDC PRIVATE nijinWVDC_static)
    nijinwvdc_apply_target_options(test_nijinWVDC)
    add_test(NAME test_nijinWVDC COMMAND test_nijinWVDC)

    if(NIJINWVDC_BUILD_SHARED)
        add_executable(test_nijinWVDC_c tests/test_c_api.c)
        if(MSVC)
            target_compile_options(test_nijinWVDC_c PRIVATE /W4 /WX)
            target_compile_definitions(test_nijinWVDC_c PRIVATE _CRT_SECURE_NO_WARNINGS)
        else()
            set_target_properties(test_nijinWVDC_c PROPERTIES
                C_STANDARD 11
                C_STANDARD_REQUIRED ON
                C_EXTENSIONS OFF
            )
            target_compile_options(test_nijinWVDC_c PRIVATE -Wall -Wextra -Werror)
            if(APPLE)
                set_target_properties(test_nijinWVDC_c PROPERTIES BUILD_RPATH "@executable_path/../lib")
            elseif(UNIX)
                set_target_properties(test_nijinWVDC_c PROPERTIES BUILD_RPATH "$ORIGIN/../lib")
            endif()
        endif()
        target_link_libraries(test_nijinWVDC_c PRIVATE nijinWVDC_shared)
        if(UNIX AND NOT APPLE)
            target_link_libraries(test_nijinWVDC_c PRIVATE m)
        endif()
        add_test(NAME test_nijinWVDC_c COMMAND test_nijinWVDC_c)
    endif()
endif()

if(UNIX AND NOT APPLE)
    set(NIJINWVDC_PC_LIBS "-lpthread")
    set(NIJINWVDC_PC_LIBS_PRIVATE "-lpthread -lm")
else()
    set(NIJINWVDC_PC_LIBS "")
    set(NIJINWVDC_PC_LIBS_PRIVATE "")
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/nijinWVDCConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/nijinWVDCConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nijinWVDC"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/nijinWVDCConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/nijinWVDC.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/nijinWVDC.pc"
    @ONLY
)

install(TARGETS ${NIJINWVDC_INSTALL_TARGETS}
    EXPORT nijinWVDC-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES
    include/nijinWVDC.h
    include/nijinWVDC_c.h
    include/nijinWVDC_export.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(FILES
    LICENSE
    README.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/nijinWVDC"
)
install(EXPORT nijinWVDC-targets
    FILE nijinWVDC-targets.cmake
    NAMESPACE nijinWVDC::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nijinWVDC"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/nijinWVDCConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/nijinWVDCConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nijinWVDC"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/nijinWVDC.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
