cmake_minimum_required(VERSION 3.21)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(goDragonCooker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" APP_VERSION LIMIT_COUNT 1)

# CPU baseline. x64 requires x86-64-v3 (AVX2, FMA, BMI2); Windows ARM64 requires armv8.2
# (the Windows 11 on ARM floor); Linux ARM64 stays armv8-a and macOS stays apple-m1.
if(MSVC)
    string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" GDC_CPU)
elseif(CMAKE_OSX_ARCHITECTURES)
    string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" GDC_CPU)
else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" GDC_CPU)
endif()
set(GDC_X64 OFF)
if(GDC_CPU MATCHES "^(x86_64|amd64|x64)$")
    set(GDC_X64 ON)
endif()
option(GDC_X86_64_V3 "Require x86-64-v3 (AVX2, FMA, BMI2) on x64" ${GDC_X64})
if(NOT GDC_X64)
    set(GDC_X86_64_V3 OFF)
endif()

# Built before the global arch flags so it can still report a missing feature on an older CPU.
add_library(gdc_cpu_check OBJECT app/core/cpu_check.cpp)
if(GDC_X86_64_V3)
    target_compile_definitions(gdc_cpu_check PRIVATE GDC_REQUIRE_X86_64_V3)
endif()

if(GDC_X86_64_V3)
    if(MSVC)
        add_compile_options(/arch:AVX2)
    else()
        add_compile_options(-march=x86-64-v3)
        if(MINGW)
            # GCC does not keep the Win64 stack 32-byte aligned for AVX spills (GCC bug 54412).
            add_compile_options(-Wa,-muse-unaligned-vector-move)
        endif()
    endif()
elseif(MSVC AND GDC_CPU STREQUAL "arm64")
    add_compile_options(/arch:armv8.2)
endif()
if(NOT MSVC)
    # Keep float results identical to MSVC /fp:precise (no fused multiply-add).
    add_compile_options(-ffp-contract=off)
endif()
message(STATUS "goDragonCooker CPU: ${GDC_CPU}, x86-64-v3: ${GDC_X86_64_V3}")

include(FetchContent)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(MSGPACK_USE_BOOST OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(msgpack
    URL https://github.com/msgpack/msgpack-c/releases/download/cpp-7.0.0/msgpack-cxx-7.0.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    SOURCE_SUBDIR build/cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(glfw imgui json msgpack zstd)

# Assimp v6.0.5 (game uses 6.0.5-collada-parity), static, with its bundled Draco (encoder included).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_CRT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_VIEW OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_IGNORE_GIT_HASH ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DRACO ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DRACO_STATIC ON CACHE BOOL "" FORCE)
# Exporter fixes that are not on assimp/assimp. CI checks out the upstream commit, so apply them here.
set(_assimp_patch "${CMAKE_CURRENT_SOURCE_DIR}/third_party/assimp-exporters.patch")
execute_process(
    COMMAND git apply --ignore-whitespace --reverse --check "${_assimp_patch}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/assimp"
    RESULT_VARIABLE _assimp_already
    OUTPUT_QUIET ERROR_QUIET)
if(NOT _assimp_already EQUAL 0)
    execute_process(
        COMMAND git apply --ignore-whitespace "${_assimp_patch}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/assimp"
        RESULT_VARIABLE _assimp_apply
        ERROR_VARIABLE _assimp_err)
    if(NOT _assimp_apply EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${_assimp_patch}: ${_assimp_err}")
    endif()
endif()
add_subdirectory(third_party/assimp EXCLUDE_FROM_ALL)
# Draco generates draco/draco_features.h under the top-level binary dir, which Assimp only finds when it is the root project.
target_include_directories(assimp PRIVATE "${CMAKE_BINARY_DIR}")
if(TARGET draco_static)
    set(GDC_DRACO_TARGET draco_static)
else()
    set(GDC_DRACO_TARGET draco)
endif()

# The bundled zlib (assimp contrib/zlib) ships a broken `#define fdopen(fd,mode) NULL`
# macro for TARGET_OS_MAC in zutil.h. On Apple platforms TARGET_OS_MAC is always defined,
# so the macro rewrites the real fdopen() declaration in <stdio.h> into a syntax error
# under newer Apple Clang / the Xcode 16.4 SDK. Defining fdopen as an identity macro makes
# the `#ifndef fdopen` guard in zutil.h skip the broken definition, while leaving the real
# function declaration and any call sites untouched. Harmless if upstream ever fixes it.
if(APPLE)
    target_compile_definitions(zlibstatic PRIVATE fdopen=fdopen)
endif()

add_library(gdc_headers INTERFACE)
target_include_directories(gdc_headers SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bcdec"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Release optimizations for first-party targets only; dependencies keep their own flags.
# Subdirectories copied CMAKE_<LANG>_FLAGS_RELEASE when they were added, so this only reaches
# targets defined in this file.
include(CheckIPOSupported)
check_ipo_supported(RESULT GDC_IPO_SUPPORTED OUTPUT GDC_IPO_ERROR LANGUAGES C CXX)
if(NOT GDC_IPO_SUPPORTED)
    message(STATUS "Link-time optimization unavailable: ${GDC_IPO_ERROR}")
endif()
if(MSVC)
    foreach(lang C CXX)
        string(REPLACE "/Ob2" "/Ob3" CMAKE_${lang}_FLAGS_RELEASE "${CMAKE_${lang}_FLAGS_RELEASE}")
    endforeach()
endif()

# Warnings, platform defines and optimizations shared by every first-party target.
function(gdc_target_options target)
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/app")
    if(WIN32)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4 /utf-8 $<$<CONFIG:Release>:/Gw>)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra
            $<$<CONFIG:Release>:-ffunction-sections -fdata-sections>)
    endif()
    if(GDC_IPO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        target_link_libraries(${target} PRIVATE gdc_cpu_check)
        if(APPLE)
            target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-Wl,-dead_strip>)
        elseif(NOT MSVC)
            target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
        endif()
    endif()
endfunction()

# Translations: Locale files (app/locales/<locale>/main.translation.json), embedded as byte
# arrays like the icon. The order here is the order of the language list in app/core/i18n.cpp.
set(GDC_LOCALES en-US pl_PL es_ES de_DE it_IT)
set(GDC_LOCALE_ARRAYS "")
set(GDC_LOCALE_TABLE "")
set(GDC_LOCALE_INDEX 0)
foreach(locale ${GDC_LOCALES})
    set(locale_file "${CMAKE_CURRENT_SOURCE_DIR}/app/locales/${locale}/main.translation.json")
    file(READ "${locale_file}" locale_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," locale_bytes "${locale_hex}")
    string(LENGTH "${locale_hex}" locale_hex_length)
    math(EXPR locale_size "${locale_hex_length} / 2")
    string(APPEND GDC_LOCALE_ARRAYS "static const unsigned char locale${GDC_LOCALE_INDEX}[] = {${locale_bytes}};\n")
    string(APPEND GDC_LOCALE_TABLE "    {locale${GDC_LOCALE_INDEX}, ${locale_size}},\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${locale_file}")
    math(EXPR GDC_LOCALE_INDEX "${GDC_LOCALE_INDEX} + 1")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/locales_data.h"
    "#pragma once\n#include <cstddef>\n${GDC_LOCALE_ARRAYS}"
    "struct LocaleData { const unsigned char* data; size_t size; };\n"
    "static const LocaleData kLocaleData[] = {\n${GDC_LOCALE_TABLE}};\n"
    "static const size_t kLocaleDataCount = ${GDC_LOCALE_INDEX};\n")

# Strings, paths, background tasks, settings. No UI, no codecs.
add_library(gdc_core STATIC
    app/core/strings.cpp
    app/core/platform.cpp
    app/core/task.cpp
    app/core/settings.cpp
    app/core/i18n.cpp)
gdc_target_options(gdc_core)
target_link_libraries(gdc_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads ${CMAKE_DL_LIBS})
target_include_directories(gdc_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# OpenEXR reads normal maps, including DWAA. Same pin as app/native/vulkan.
# Force the vendored copies: a system Imath/libdeflate from another toolchain
# (for example MSYS2) is not linkable from this build.
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_IMATH ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_DEFLATE ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_OPENJPH ON CACHE BOOL "" FORCE)
unset(Imath_FOUND CACHE)
unset(Imath_DIR CACHE)
FetchContent_Declare(
    openexr
    URL https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v3.4.14.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(openexr)

# Texture codecs (texconv, Compressonator Vulkan) and cook rules.
add_library(gdc_texture STATIC
    app/texture/imageheader.cpp
    app/texture/texconv.cpp
    app/texture/vulkan_codec.cpp
    app/texture/cook_rules.cpp
    app/texture/cook_validate.cpp
    app/texture/cook_retry.cpp
    app/texture/normal_space.cpp)
gdc_target_options(gdc_texture)
target_link_libraries(gdc_texture PUBLIC gdc_core PRIVATE gdc_headers OpenEXR::OpenEXR)

# TSShape IO (DTS/CDAE), Assimp bridge, format registry, validation.
file(GLOB_RECURSE GDC_MODEL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/model/*.cpp")
add_library(gdc_model STATIC ${GDC_MODEL_SOURCES})
gdc_target_options(gdc_model)
target_compile_definitions(gdc_model PRIVATE MSGPACK_NO_BOOST)
target_link_libraries(gdc_model PUBLIC gdc_core PRIVATE msgpack-cxx libzstd_static assimp::assimp ${GDC_DRACO_TARGET})
target_include_directories(gdc_model PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/assimp/contrib/draco/src"
    "${CMAKE_BINARY_DIR}")

option(GDC_BUILD_TESTS "Build gdc_i18n_test, gdc_model_test and gdc_cook_test" ON)
if(GDC_BUILD_TESTS)
    enable_testing()

    add_executable(gdc_model_test app/tests/model_test.cpp)
    gdc_target_options(gdc_model_test)
    target_link_libraries(gdc_model_test PRIVATE gdc_model)

    add_executable(gdc_i18n_test app/tests/i18n_test.cpp)
    gdc_target_options(gdc_i18n_test)
    target_link_libraries(gdc_i18n_test PRIVATE gdc_model)

    add_executable(gdc_cook_test app/tests/cook_test.cpp)
    gdc_target_options(gdc_cook_test)
    target_link_libraries(gdc_cook_test PRIVATE gdc_texture)

    set(GDC_TEST_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/app/tests/data/triangle.obj")
    set(GDC_TEST_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/app/tests/data/color.png")
    add_test(NAME gdc_i18n_test COMMAND gdc_i18n_test)
    add_test(NAME gdc_model_test COMMAND gdc_model_test
        --convert glb,glb-draco,dae,cdae,dts
        --out "${CMAKE_CURRENT_BINARY_DIR}/selftest-model"
        --back "${GDC_TEST_MODEL}")
    add_test(NAME gdc_cook_test COMMAND gdc_cook_test
        --work "${CMAKE_CURRENT_BINARY_DIR}/selftest-cook"
        "color:${GDC_TEST_IMAGE}" "normal:${GDC_TEST_IMAGE}" "dataR:${GDC_TEST_IMAGE}")
    add_test(NAME gdc_cook_corrupt COMMAND gdc_cook_test
        --work "${CMAKE_CURRENT_BINARY_DIR}/selftest-cook-corrupt"
        --corrupt "color:${GDC_TEST_IMAGE}")
    set_tests_properties(gdc_cook_test gdc_cook_corrupt PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/app/dragonpbr.ico" ICON_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ICON_BYTES "${ICON_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/app_icon.h"
    "static const unsigned char appIcon[] = {${ICON_BYTES}};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/dragonpbr.ico")

add_executable(goDragonCooker WIN32
    app/main.cpp
    app/ui/theme.cpp
    app/ui/widgets.cpp
    app/gpu/gpu.cpp
    app/tabs/cook_tab.cpp
    app/tabs/matgen_tab.cpp
    app/tabs/model_tab.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
gdc_target_options(goDragonCooker)

target_include_directories(goDragonCooker PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(goDragonCooker PRIVATE APP_VERSION="${APP_VERSION}")

target_link_libraries(goDragonCooker PRIVATE gdc_texture gdc_model glfw OpenGL::GL)

if(WIN32)
    target_link_libraries(goDragonCooker PRIVATE dxgi dwmapi)
endif()
if(APPLE)
    target_compile_definitions(goDragonCooker PRIVATE GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    target_link_options(goDragonCooker PRIVATE /ENTRY:mainCRTStartup)
else()
    if(NOT APPLE)
        target_link_options(goDragonCooker PRIVATE $<$<CONFIG:Release>:-s>)
    endif()
    if(MINGW)
        target_link_options(goDragonCooker PRIVATE -static)
    elseif(NOT APPLE)
        target_link_options(goDragonCooker PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endif()
