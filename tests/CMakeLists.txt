include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

add_executable(avernal-rhi-d3d12-tests d3d12_test.cpp)
target_link_libraries(avernal-rhi-d3d12-tests PRIVATE avernal::rhi_d3d12 GTest::gtest_main)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(avernal-rhi-d3d12-tests PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(avernal-rhi-d3d12-tests PRIVATE /W4)
endif()

include(GoogleTest)
gtest_discover_tests(avernal-rhi-d3d12-tests DISCOVERY_MODE PRE_TEST)
