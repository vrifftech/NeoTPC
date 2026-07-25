# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library" FORCE)

project(NeoTPC VERSION 1.0.0 DESCRIPTION "C++17/wxWidgets Odyssey texture viewer and converter" LANGUAGES CXX)
set(NEOSHARED_ROOT "" CACHE PATH
    "Path to the neoshared source repository; defaults to ../neoshared")

if(NOT NEOSHARED_ROOT AND DEFINED ENV{NEOSHARED_ROOT}
   AND NOT "$ENV{NEOSHARED_ROOT}" STREQUAL "")
    set(NEOSHARED_ROOT "$ENV{NEOSHARED_ROOT}" CACHE PATH
        "Path to the neoshared source repository" FORCE)
endif()

if(NOT NEOSHARED_ROOT)
    get_filename_component(NEOSHARED_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../neoshared" ABSOLUTE)
else()
    get_filename_component(NEOSHARED_ROOT "${NEOSHARED_ROOT}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
endif()
set(NEOSHARED_ROOT "${NEOSHARED_ROOT}" CACHE PATH
    "Path to the neoshared source repository" FORCE)

if(NOT EXISTS "${NEOSHARED_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "neoshared was not found at '${NEOSHARED_ROOT}'. "
        "Clone the neoshared repository beside this repository, or configure "
        "with -DNEOSHARED_ROOT=/path/to/neoshared.")
endif()

if(MSVC)
    foreach(_neo_runtime_flag_var IN ITEMS
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if(DEFINED ${_neo_runtime_flag_var})
            string(REGEX REPLACE "(^|[ /-])MDd?($|[ ]|/)" "\\1MT\\2" ${_neo_runtime_flag_var} "${${_neo_runtime_flag_var}}")
        endif()
    endforeach()
endif()

function(neo_force_static_msvc_runtime target_name)
    if(MSVC AND TARGET ${target_name})
        set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:/MTd>
            $<$<NOT:$<CONFIG:Debug>>:/MT>
        )
    endif()
endfunction()

function(neo_force_static_msvc_runtime_for_directory_targets)
    if(MSVC)
        get_property(_neo_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_neo_target IN LISTS _neo_targets)
            get_target_property(_neo_type ${_neo_target} TYPE)
            if(NOT _neo_type STREQUAL "INTERFACE_LIBRARY" AND NOT _neo_type STREQUAL "UTILITY")
                neo_force_static_msvc_runtime(${_neo_target})
            endif()
        endforeach()
    endif()
endfunction()

include(GNUInstallDirs)

message(STATUS "Using neoshared: ${NEOSHARED_ROOT}")
include("${NEOSHARED_ROOT}/cmake/NeoMinimalRelease.cmake")
if(NOT TARGET neoshared::wx)
    add_subdirectory("${NEOSHARED_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/neoshared" EXCLUDE_FROM_ALL)
endif()
# Codec implementations are external dependencies. The vcpkg manifest installs
# the pinned dependency graph automatically when the vcpkg toolchain is active.
find_package(SPNG CONFIG REQUIRED)
find_package(JPEG REQUIRED)

if(TARGET spng::spng)
    set(NEOTPC_SPNG_TARGET spng::spng)
elseif(TARGET spng::spng_static)
    set(NEOTPC_SPNG_TARGET spng::spng_static)
else()
    message(FATAL_ERROR
        "libspng was found, but its package exported neither spng::spng "
        "nor spng::spng_static.")
endif()

if(NOT TARGET JPEG::JPEG)
    message(FATAL_ERROR "The JPEG package did not export the required JPEG::JPEG target.")
endif()

add_library(neotpc_texture
    src/texture/BatchConverter.cpp
    src/texture/FileUtil.cpp
    src/texture/Image.cpp
    src/texture/InternalImageCodecs.cpp
    src/texture/Txi.cpp
)
target_include_directories(neotpc_texture
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(neotpc_texture PRIVATE
    ${NEOTPC_SPNG_TARGET}
    JPEG::JPEG
)
target_compile_features(neotpc_texture PUBLIC cxx_std_17)
target_compile_options(neotpc_texture PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)

add_library(neotpc_core src/core/TextureDocument.cpp)
target_include_directories(neotpc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neotpc_core PUBLIC neotpc_texture)
target_compile_features(neotpc_core PUBLIC cxx_std_17)
target_compile_options(neotpc_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)
target_compile_definitions(neotpc_core PUBLIC NEOTPC_VERSION="${PROJECT_VERSION}")

option(NEOTPC_BUILD_CLI "Build the NeoTPC command-line utility" ON)
if(NEOTPC_BUILD_CLI)
    add_executable(neotpc-cli src/cli/main.cpp)
    target_link_libraries(neotpc-cli PRIVATE neotpc_core)
    target_compile_features(neotpc-cli PRIVATE cxx_std_17)
    install(TARGETS neotpc-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

option(NEOTPC_BUILD_WX_GUI "Build the wxWidgets NeoTPC desktop GUI when wxWidgets is available" ON)
option(NEOTPC_REQUIRE_WX_GUI "Fail configure if wxWidgets is unavailable while NEOTPC_BUILD_WX_GUI is ON" OFF)

function(neotpc_link_wx target_name)
    target_compile_definitions(${target_name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set(_wx_targets)
    foreach(_wx_target IN ITEMS wx::core wx::base wx::adv wxWidgets::wxWidgets)
        if(TARGET ${_wx_target})
            list(APPEND _wx_targets ${_wx_target})
        endif()
    endforeach()
    if(_wx_targets)
        target_link_libraries(${target_name} PRIVATE ${_wx_targets})
    else()
        if(wxWidgets_USE_FILE)
            include(${wxWidgets_USE_FILE})
        endif()
        target_include_directories(${target_name} SYSTEM PRIVATE ${wxWidgets_INCLUDE_DIRS})
        target_compile_definitions(${target_name} PRIVATE ${wxWidgets_DEFINITIONS})
        target_link_libraries(${target_name} PRIVATE ${wxWidgets_LIBRARIES})
    endif()
    neo_force_static_msvc_runtime(${target_name})
endfunction()

if(NEOTPC_BUILD_WX_GUI)
    set(NEOTPC_WX_COMPONENTS core base adv)
    set(_neo_prefer_wx_config_package OFF)
    if(WIN32 OR DEFINED VCPKG_TARGET_TRIPLET OR CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake")
        set(_neo_prefer_wx_config_package ON)
    endif()
    if(_neo_prefer_wx_config_package)
        find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOTPC_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET)
        endif()
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOTPC_WX_COMPONENTS})
        endif()
    else()
        find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOTPC_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOTPC_WX_COMPONENTS})
        endif()
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET)
        endif()
    endif()

    if(wxWidgets_FOUND OR TARGET wx::core OR TARGET wxWidgets::wxWidgets)
        find_package(Threads REQUIRED)
        set(NEOTPC_WX_SOURCES
            src/wx/BatchDialog.cpp
            src/wx/EncodingOptionsPanel.cpp
            src/wx/TextureCanvas.cpp
            src/wx/main.cpp
        )
        if(WIN32)
            list(APPEND NEOTPC_WX_SOURCES resources/neotpc.rc)
        endif()
        if(WIN32)
            add_executable(NeoTPC WIN32 ${NEOTPC_WX_SOURCES})
        elseif(APPLE)
            add_executable(NeoTPC MACOSX_BUNDLE ${NEOTPC_WX_SOURCES})
        else()
            add_executable(NeoTPC ${NEOTPC_WX_SOURCES})
        endif()
        target_link_libraries(NeoTPC PRIVATE neotpc_core neoshared::wx Threads::Threads)
        target_include_directories(NeoTPC PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/wx ${CMAKE_CURRENT_SOURCE_DIR}/resources)
        neotpc_link_wx(NeoTPC)
        target_compile_features(NeoTPC PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(NeoTPC PRIVATE /W4 /permissive- /utf-8)
        else()
            target_compile_options(NeoTPC PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        install(TARGETS NeoTPC BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

        if(UNIX AND NOT APPLE)
            foreach(NEO_ICON_SIZE 16 24 32 48 64 128 256)
                install(FILES resources/icons/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/neotpc.png
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/apps
                    RENAME neotpc.png)
            endforeach()
            configure_file(resources/neotpc.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/neotpc.desktop @ONLY)
            install(FILES ${CMAKE_CURRENT_BINARY_DIR}/neotpc.desktop
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
        endif()
    elseif(NEOTPC_REQUIRE_WX_GUI)
        message(FATAL_ERROR "wxWidgets was not found, but NEOTPC_REQUIRE_WX_GUI is ON. Configure with the vcpkg toolchain on Windows, or install the wxWidgets development package on this platform.")
    else()
        message(WARNING "wxWidgets was not found. NeoTPC core and CLI targets will still build.")
    endif()
endif()

option(NEOTPC_BUILD_TESTS "Build NeoTPC codec regression tests" OFF)
if(NEOTPC_BUILD_TESTS)
    enable_testing()
    add_executable(neotpc-codec-tests tests/codec_tests.cpp)
    target_link_libraries(neotpc-codec-tests PRIVATE neotpc_core)
    target_compile_definitions(neotpc-codec-tests PRIVATE
        NEOTPC_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME neotpc-codec-roundtrip COMMAND neotpc-codec-tests)
    if(TARGET neotpc-cli)
        add_test(NAME neotpc-cli-help COMMAND neotpc-cli --help)
        add_test(NAME neotpc-cli-fixture-info COMMAND neotpc-cli info ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test.tpc)
    endif()
endif()

set(NEOTPC_DOC_DIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/NeoTPC")
install(FILES README.md LICENSE NOTICE.md THIRD_PARTY_NOTICES.md DESTINATION ${NEOTPC_DOC_DIR})

# vcpkg stores the authoritative license text for every installed port. Copy
# those files into vcpkg-built distributions without making them source-tree
# dependencies or requiring them for system-package builds.
function(neotpc_install_vcpkg_copyright package_name output_name)
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(_copyright
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${package_name}/copyright")
        if(EXISTS "${_copyright}")
            install(FILES "${_copyright}"
                DESTINATION "${NEOTPC_DOC_DIR}/third-party"
                RENAME "${output_name}")
        endif()
    endif()
endfunction()

neotpc_install_vcpkg_copyright(libspng libspng-LICENSE.txt)
neotpc_install_vcpkg_copyright(libjpeg-turbo libjpeg-turbo-LICENSE.txt)
neotpc_install_vcpkg_copyright(zlib zlib-LICENSE.txt)
neotpc_install_vcpkg_copyright(wxwidgets wxWidgets-LICENSE.txt)

neo_force_static_msvc_runtime_for_directory_targets()
neo_apply_minimal_release_to_directory_targets()
