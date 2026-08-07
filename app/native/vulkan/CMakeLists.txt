cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
project(gdc_vulkan LANGUAGES CXX)

find_package(Vulkan REQUIRED)

include(FetchContent)

find_package(TIFF QUIET)
if(NOT TIFF_FOUND)
    set(tiff-tools OFF CACHE BOOL "" FORCE)
    set(tiff-tests OFF CACHE BOOL "" FORCE)
    set(tiff-contrib OFF CACHE BOOL "" FORCE)
    set(tiff-docs OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        tiff
        URL https://github.com/libsdl-org/libtiff/archive/refs/tags/v4.7.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(tiff)
    add_library(TIFF::TIFF ALIAS tiff)
endif()

find_package(OpenEXR QUIET)
if(NOT OpenEXR_FOUND)
    set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(OPENEXR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        openexr
        URL https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v3.4.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(openexr)
endif()

set(COMPRESSONATOR_SHADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/compressonator/cmp_core/shaders")
if(NOT EXISTS "${COMPRESSONATOR_SHADERS}")
    set(COMPRESSONATOR_SHADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/compressonator/cmp_core/shaders")
endif()

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")
set(SHADER_OUTPUTS)

if(GDC_PRECOMPILED_SHADER_DIR)
    foreach(name bc4 bc5 bc6 bc7 mipmap)
        set(source "${GDC_PRECOMPILED_SHADER_DIR}/${name}.spv")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Precompiled shader not found: ${source}")
        endif()
        set(output "${SHADER_OUTPUT_DIR}/${name}.spv")
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source}" "${output}"
            DEPENDS "${source}"
            VERBATIM)
        list(APPEND SHADER_OUTPUTS "${output}")
    endforeach()
else()
    find_program(DXC_EXECUTABLE NAMES dxc HINTS "$ENV{VULKAN_SDK}/Bin")
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "dxc is required to compile the Vulkan BCn shaders")
    endif()

    function(add_bcn_shader name source)
        set(entry_point EncodeBlocks)
        if(ARGC GREATER 2)
            set(entry_point "${ARGV2}")
        endif()
        set(output "${SHADER_OUTPUT_DIR}/${name}.spv")
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${DXC_EXECUTABLE}"
                -spirv -T cs_6_0 -HV 2018 -E "${entry_point}"
                -fspv-target-env=vulkan1.1 -fvk-use-dx-layout
                -fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-u-shift 3 0
                -I "${COMPRESSONATOR_SHADERS}"
                -Fo "${output}" "${source}"
            DEPENDS "${source}"
            VERBATIM)
        set(SHADER_OUTPUTS ${SHADER_OUTPUTS} "${output}" PARENT_SCOPE)
    endfunction()

    add_bcn_shader(bc4 "${COMPRESSONATOR_SHADERS}/bc4_encode_kernel.hlsl")
    add_bcn_shader(bc5 "${COMPRESSONATOR_SHADERS}/bc5_encode_kernel.hlsl")
    add_bcn_shader(bc6 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bc6_vulkan.hlsl")
    add_bcn_shader(bc7 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bc7_vulkan.hlsl")
    add_bcn_shader(mipmap "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mipmap.hlsl" GenerateMip)
endif()
add_custom_target(gdc_vulkan_shaders ALL DEPENDS ${SHADER_OUTPUTS})

add_library(gdc_vulkan SHARED gdc_vulkan.cpp)
if(WIN32)
    set_target_properties(gdc_vulkan PROPERTIES PREFIX "")
elseif(UNIX)
    set_target_properties(gdc_vulkan PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN")
    target_link_options(gdc_vulkan PRIVATE "-Wl,--disable-new-dtags")
endif()
add_dependencies(gdc_vulkan gdc_vulkan_shaders)
target_compile_features(gdc_vulkan PRIVATE cxx_std_17)
target_compile_definitions(gdc_vulkan PRIVATE STB_IMAGE_IMPLEMENTATION STBI_FAILURE_USERMSG)
target_include_directories(gdc_vulkan PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(gdc_vulkan SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/compressonator/external/stb")
if(openexr_SOURCE_DIR)
    target_include_directories(gdc_vulkan SYSTEM PRIVATE
        "${openexr_SOURCE_DIR}/src/lib")
endif()
target_link_libraries(gdc_vulkan PRIVATE Vulkan::Vulkan TIFF::TIFF OpenEXR::OpenEXR)

if(MSVC)
    target_compile_options(gdc_vulkan PRIVATE /W4 /EHsc)
else()
    target_compile_options(gdc_vulkan PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    if(MINGW)
        target_link_options(gdc_vulkan PRIVATE
            -static-libgcc
            -static-libstdc++
            -Wl,--no-insert-timestamp)
        target_link_libraries(gdc_vulkan PRIVATE -Wl,-Bstatic winpthread -Wl,-Bdynamic)
    endif()
endif()

get_filename_component(COMPILER_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)
install(TARGETS gdc_vulkan
    RUNTIME_DEPENDENCY_SET gdc_vulkan_dependencies
    RUNTIME DESTINATION . COMPONENT gdc_vulkan_runtime
    LIBRARY DESTINATION . COMPONENT gdc_vulkan_runtime)
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME gdc_vulkan_runtime)
install(RUNTIME_DEPENDENCY_SET gdc_vulkan_dependencies
        DIRECTORIES "${COMPILER_DIRECTORY}"
        PRE_EXCLUDE_REGEXES "api-ms-" "ext-ms-"
        POST_EXCLUDE_REGEXES
            ".*[Ss]ystem32/.*"
            ".*vulkan-1\\.dll"
            ".*/libvulkan\\.so.*"
            ".*/ld-linux.*"
            ".*/lib(c|m|pthread|dl|rt)\\.so.*"
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .)
install(FILES ${SHADER_OUTPUTS}
    DESTINATION shaders
    COMPONENT gdc_vulkan_runtime)
