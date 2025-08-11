cmake_minimum_required(VERSION 3.10.0)
project(nats-connector)
set (PROJECT_LIBS "${PROJECT_NAME}-libs")
set (PROJECT_TESTS "${PROJECT_NAME}-tests")

set(ENABLE_TESTS OFF CACHE BOOL "Build unit tests" FORCE) # ON/OFF option only. OFF by default.
# FORCE here so we don't need to delete "build" folder every time we change this option
# (because otherwise CMake would not reconfigure the project to include tests)

# include nats.c library into our project - doing it by automatically fetching latest stable version from GitHub
#set(NATS_BUILD_STREAMING OFF CACHE BOOL "Disable NATS Streaming build")
#include(FetchContent)
#FetchContent_Declare(
#  nats
#  GIT_REPOSITORY https://github.com/nats-io/nats.c
#  GIT_TAG v3.10.1  # latest stable tag on 04/08/2025
#)
#FetchContent_MakeAvailable(nats)

# initial version nats.c library used in this project is 3.10.1
# line used if we already have nats.c library installed on the system (e.g. via vcpkg)
find_package(cnats CONFIG REQUIRED)

# Download & build GoogleTest
if(ENABLE_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest
        GIT_TAG v1.17.0  # latest stable tag on 08/08/2025
    )
    FetchContent_MakeAvailable(googletest)
endif()



# Main application
add_library(${PROJECT_LIBS}
    src/MathPicker.cpp
    src/NatsManager.cpp
)
target_include_directories(${PROJECT_LIBS} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_LIBS} PUBLIC cnats::nats)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_LIBS})



# Building unit tests sources
if(ENABLE_TESTS)
    # line to enable testing support (grants ability to use commands like `gtest_discover_tests`)
    enable_testing()

    set(PROJECT_TESTS_SOURCES
        tests/MathPickerTest.cpp
        tests/NatsManagerTest.cpp
        tests/StderrCapture.cpp
    )

    add_executable(${PROJECT_TESTS} ${PROJECT_TESTS_SOURCES})
    target_link_libraries(${PROJECT_TESTS} PRIVATE gtest_main ${PROJECT_LIBS})

    include(GoogleTest)
    gtest_discover_tests(${PROJECT_TESTS})
endif()