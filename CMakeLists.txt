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
FetchContent_MakeAvailable(glfw imgui json)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Warnings and platform defines shared by every first-party target.
function(gdc_target_options target)
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/app")
    if(WIN32)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Strings, paths, background tasks, settings. No UI, no codecs.
add_library(gdc_core STATIC
    app/core/strings.cpp
    app/core/platform.cpp
    app/core/task.cpp
    app/core/settings.cpp)
gdc_target_options(gdc_core)
target_link_libraries(gdc_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads ${CMAKE_DL_LIBS})

# Texture codecs (texconv, Compressonator Vulkan) and cook rules.
add_library(gdc_texture STATIC
    app/texture/imageheader.cpp
    app/texture/texconv.cpp
    app/texture/vulkan_codec.cpp
    app/texture/cook_rules.cpp)
gdc_target_options(gdc_texture)
target_link_libraries(gdc_texture PUBLIC gdc_core)

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

target_link_libraries(goDragonCooker PRIVATE gdc_texture glfw OpenGL::GL)

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
