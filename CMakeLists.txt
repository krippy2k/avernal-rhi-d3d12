cmake_minimum_required(VERSION 3.28)

project(
    avernal-rhi-d3d12
    VERSION 0.1.0
    DESCRIPTION "Direct3D 12 backend for the Avernal rendering hardware interface"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "avernal-rhi-d3d12 requires Windows")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(FetchContent)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../avernal-rhi/CMakeLists.txt")
    set(AVERNAL_RHI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../avernal-rhi")
else()
    set(AVERNAL_RHI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../avernal-rhi/avernal-rhi")
endif()

if(NOT EXISTS "${AVERNAL_RHI_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "avernal-rhi not found at:\n"
        "  ${AVERNAL_RHI_SOURCE_DIR}\n"
        "Place avernal-rhi as a sibling of this project, or set "
        "FETCHCONTENT_SOURCE_DIR_AVERNAL_RHI.")
endif()

FetchContent_Declare(
    avernal_rhi
    SOURCE_DIR "${AVERNAL_RHI_SOURCE_DIR}"
    EXCLUDE_FROM_ALL
)

set(AVERNAL_RHI_BUILD_DEMO OFF)
set(AVERNAL_CORE_BUILD_DEMO OFF)

FetchContent_MakeAvailable(avernal_rhi)

add_library(avernal-rhi-d3d12)
add_library(avernal::rhi_d3d12 ALIAS avernal-rhi-d3d12)

target_sources(
    avernal-rhi-d3d12
    PRIVATE src/device.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/avernal/rhi/d3d12.hpp
)

target_link_libraries(avernal-rhi-d3d12 PUBLIC avernal::rhi PRIVATE d3d12 dxgi dxguid d3dcompiler)
target_compile_features(avernal-rhi-d3d12 PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(avernal-rhi-d3d12 PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(avernal-rhi-d3d12 PRIVATE -Wno-language-extension-token)
    endif()
elseif(MSVC)
    target_compile_options(avernal-rhi-d3d12 PRIVATE /W4)
endif()

option(AVERNAL_RHI_D3D12_BUILD_DEMO "Build the Avernal D3D12 RHI demo executable" ON)

if(AVERNAL_RHI_D3D12_BUILD_DEMO)
    add_executable(avernal-rhi-d3d12-demo apps/demo/main.cpp)
    target_link_libraries(avernal-rhi-d3d12-demo PRIVATE avernal::rhi_d3d12)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(avernal-rhi-d3d12-demo PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(avernal-rhi-d3d12-demo PRIVATE /W4)
    endif()
endif()

if(PROJECT_IS_TOP_LEVEL)
    option(AVERNAL_RHI_D3D12_BUILD_TESTS "Build Avernal D3D12 RHI unit tests" ON)
else()
    option(AVERNAL_RHI_D3D12_BUILD_TESTS "Build Avernal D3D12 RHI unit tests" OFF)
endif()

if(AVERNAL_RHI_D3D12_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
