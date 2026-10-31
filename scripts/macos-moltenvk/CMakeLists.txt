cmake_minimum_required(VERSION 3.18)
project(LunariaMoltenVKRepair LANGUAGES C CXX OBJC OBJCXX)

# Build the same components embedded in the pinned MoltenVK release, using
# local source paths. Do not let upstream's CPM recipes resolve other versions.
if(NOT LUNARIA_MVK_SOURCES OR NOT LUNARIA_MVK_FINGERPRINT)
    message(FATAL_ERROR "Run scripts/build-macos-moltenvk.sh")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_NAMESPACE_OVERRIDE MVK_spirv_cross CACHE STRING "" FORCE)
add_subdirectory("${LUNARIA_MVK_SOURCES}/SPIRV-Cross" cross)
add_library(SPRIV-Cross INTERFACE)
add_library(SPRIV-Cross::SPRIV-Cross ALIAS SPRIV-Cross)
target_link_libraries(SPRIV-Cross INTERFACE spirv-cross-core spirv-cross-reflect
                      spirv-cross-glsl spirv-cross-msl)

set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV-Headers_SOURCE_DIR "${LUNARIA_MVK_SOURCES}/SPIRV-Headers")
add_subdirectory("${LUNARIA_MVK_SOURCES}/SPIRV-Tools" tools)
add_library(SPIRV-Tools::SPIRV-Tools ALIAS SPIRV-Tools-static)

add_library(cereal INTERFACE)
add_library(cereal::cereal ALIAS cereal)
target_include_directories(cereal INTERFACE "${LUNARIA_MVK_SOURCES}/cereal/include")
add_library(VulkanHeaders INTERFACE)
add_library(Vulkan::Headers ALIAS VulkanHeaders)
target_include_directories(VulkanHeaders INTERFACE
                           "${LUNARIA_MVK_SOURCES}/Vulkan-Headers/include")

set(MVK_USE_METAL_PRIVATE_API ON CACHE BOOL "" FORCE)
set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_BINARY_DIR}/source-cache" CACHE PATH "" FORCE)
add_subdirectory("${LUNARIA_MVK_SOURCES}/MoltenVK" moltenvk)
find_library(CORE_GRAPHICS CoreGraphics REQUIRED)
target_link_libraries(MoltenVK PRIVATE ${CORE_GRAPHICS})

# Upstream uses this value in its pipeline-cache UUID. Source archives have no
# Git metadata, and using the enclosing application's Git revision is wrong.
# Identify the repaired build by its source/patch/build-configuration digest.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/moltenvk/MoltenVK/mvkGitRevDerived.h"
     "// Lunaria driver build fingerprint, used for pipeline-cache identity.\nstatic const char* mvkRevString = \"${LUNARIA_MVK_FINGERPRINT}\";\n")
