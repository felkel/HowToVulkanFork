# Copyright (c) 2025-2026, Sascha Willems
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20 FATAL_ERROR)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
include(FetchContent)
set(NAME HowToVulkan)
project(${NAME})

if (NOT IS_DIRECTORY $ENV{VULKAN_SDK})
    message(WARNING "Vulkan SDK not detected, if installed please set VULKAN_SDK env var or manually provide libraries")
else()
    message(STATUS "Found Vulkan SDK in $ENV{VULKAN_SDK}")
endif()

# This project builds the same tutorial twice, differing only in the windowing
# library. SDL3 ships with the Vulkan SDK, GLFW does not and is fetched below.
#   HowToVulkan     <- main.cpp       uses SDL3
#   HowToVulkanGLFW <- main-glfw.cpp  uses GLFW
# main-glfw.cpp is generated: python tools/make-glfw-variant.py
#
# SDL3, Volk, VMA, glm and Slang are taken from the SDK
include_directories(SYSTEM $ENV{VULKAN_SDK}/include)
# These are not part of the SDK
include_directories(external/tinyobj)
include_directories(external/ktx/include)
include_directories(external/ktx/other_include)

find_package (Vulkan REQUIRED)

if (NOT WIN32)
    FetchContent_Declare(GLM
        GIT_REPOSITORY https://github.com/g-truc/glm
        GIT_TAG 1.0
        GIT_SHALLOW ON
        EXCLUDE_FROM_ALL
        SYSTEM)
    FetchContent_MakeAvailable(GLM)
endif()

# GLFW is not part of the Vulkan SDK, so unlike SDL3 it is built from source.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(GLFW
    GIT_REPOSITORY https://github.com/glfw/glfw
    GIT_TAG 3.4
    GIT_SHALLOW ON
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(GLFW)

set(KTX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/ktx)
set(KTX_SOURCES
    ${KTX_DIR}/lib/texture.c
    ${KTX_DIR}/lib/hashlist.c
    ${KTX_DIR}/lib/checkheader.c
    ${KTX_DIR}/lib/swap.c
    ${KTX_DIR}/lib/memstream.c
    ${KTX_DIR}/lib/filestream.c
    ${KTX_DIR}/lib/vkloader.c)
add_library(ktx STATIC ${KTX_SOURCES})

if (TARGET Vulkan::Vulkan)
    target_link_libraries(ktx PUBLIC Vulkan::Vulkan)
elseif (APPLE)
    target_link_libraries(ktx PUBLIC $ENV{VULKAN_SDK}/lib/libvulkan.dylib)
elseif (NOT WIN32)
    target_link_libraries(ktx PUBLIC $ENV{VULKAN_SDK}/lib/libvulkan.so)
endif()

find_library(SDL_LIBRARY NAMES SDL3 HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)
find_library(Slang_LIBRARY NAMES slang slang-compiler HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)

if(WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DVK_USE_PLATFORM_WIN32_KHR")
elseif(LINUX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DVK_USE_PLATFORM_WAYLAND_KHR")
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNOMINMAX -D_USE_MATH_DEFINES")
if(MSVC)
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/")

add_executable(${NAME}     main.cpp      assets/shader.slang)
add_executable(${NAME}GLFW main-glfw.cpp assets/shader.slang)

# Everything below is identical for both variants
foreach(TUTORIAL_TARGET ${NAME} ${NAME}GLFW)
    target_compile_definitions(${TUTORIAL_TARGET} PRIVATE VK_NO_PROTOTYPES)
    if (WIN32)
        target_compile_definitions(${TUTORIAL_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set_target_properties(${TUTORIAL_TARGET} PROPERTIES DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(${TUTORIAL_TARGET} PRIVATE cxx_std_20)
    target_link_libraries(${TUTORIAL_TARGET} PRIVATE ktx ${Slang_LIBRARY})
    if (NOT WIN32)
        target_link_libraries(${TUTORIAL_TARGET} PRIVATE glm::glm-header-only)
    endif()
endforeach()

# ...and this is the only build difference between them
target_link_libraries(${NAME}     PRIVATE ${SDL_LIBRARY})
target_link_libraries(${NAME}GLFW PRIVATE glfw)
