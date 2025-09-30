cmake_minimum_required(VERSION 3.14)
project(nats-connector)
set (PROJECT_LIBS "${PROJECT_NAME}-libs")
set (PROJECT_TESTS "${PROJECT_NAME}-tests")

set(ENABLE_TESTS OFF CACHE BOOL "Build unit tests" FORCE) # ON/OFF option only. OFF by default.
# FORCE here so we don't need to delete "build" folder every time we change this option
# (because otherwise CMake would not reconfigure the project to include tests)

set(CLANG_UML_INCLUDE_ARGS "-I\"${CMAKE_CURRENT_SOURCE_DIR}/include\"")



# include nats.c library into our project - doing it by automatically fetching latest stable version from GitHub
set(NATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NATS_BUILD_STREAMING OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
  nats
  GIT_REPOSITORY https://github.com/nats-io/nats.c
  GIT_TAG v3.10.1  # latest stable tag on 04/08/2025
)
FetchContent_MakeAvailable(nats)

# line used if we already have nats.c library installed on the system (e.g. via vcpkg):
#find_package(cnats CONFIG REQUIRED)



# Global build options for Poco
set(POCO_UNBUNDLED OFF CACHE BOOL "" FORCE)
set(POCO_DISABLE_INTERNAL_OPENSSL ON CACHE BOOL "" FORCE)

# Enable only required components
set(ENABLE_FOUNDATION ON CACHE BOOL "" FORCE)
set(ENABLE_UTIL ON CACHE BOOL "" FORCE)
set(ENABLE_NET ON CACHE BOOL "" FORCE)
set(ENABLE_ENCODINGS ON CACHE BOOL "" FORCE)

# Explicitly disable OpenSSL–related modules
set(ENABLE_CRYPTO OFF CACHE BOOL "" FORCE)
set(ENABLE_JWT OFF CACHE BOOL "" FORCE)
set(ENABLE_NETSSL OFF CACHE BOOL "" FORCE)

# Disable all other components
set(ENABLE_ACTIVERECORD OFF CACHE BOOL "" FORCE)
set(ENABLE_ACTIVERECORD_COMPILER OFF CACHE BOOL "" FORCE)
set(ENABLE_DATA OFF CACHE BOOL "" FORCE)
set(ENABLE_DATA_ODBC OFF CACHE BOOL "" FORCE)
set(ENABLE_DATA_SQLITE OFF CACHE BOOL "" FORCE)
set(ENABLE_DATA_SQL_SERVER_BIG_STRINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_JSON OFF CACHE BOOL "" FORCE)
set(ENABLE_MONGODB OFF CACHE BOOL "" FORCE)
set(ENABLE_PAGECOMPILER OFF CACHE BOOL "" FORCE)
set(ENABLE_PAGECOMPILER_FILE2PAGE OFF CACHE BOOL "" FORCE)
set(ENABLE_PROMETHEUS OFF CACHE BOOL "" FORCE)
set(ENABLE_REDIS OFF CACHE BOOL "" FORCE)
set(ENABLE_XML OFF CACHE BOOL "" FORCE)
set(ENABLE_ZIP OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SAMPLES OFF CACHE BOOL "" FORCE)

# These flags needs to avoid linking errors
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "" FORCE)

include(FetchContent)
FetchContent_Declare(
    poco
    GIT_REPOSITORY https://github.com/pocoproject/poco
    GIT_TAG poco-1.14.2-release  # latest stable tag on 04/08/2025
)
FetchContent_MakeAvailable(poco)



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
    src/http_handler.cpp
    src/nats_manager.cpp
)
target_include_directories(${PROJECT_LIBS} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external
    ${nats_SOURCE_DIR}/src
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_LIBS} 
    PUBLIC 
        nats
        Poco::Net
        Poco::Encodings
        Poco::Util
        Poco::Foundation
)
target_link_libraries(${PROJECT_NAME} PUBLIC ${PROJECT_LIBS})



# Building unit tests sources
if(ENABLE_TESTS)
    # line to enable testing support (grants ability to use commands like `gtest_discover_tests`)
    enable_testing()

    set(PROJECT_TESTS_SOURCES
        tests/nats_manager_tests.cpp
        tests/std_err_capture.cpp
    )

    add_executable(${PROJECT_TESTS} ${PROJECT_TESTS_SOURCES})
    target_link_libraries(${PROJECT_TESTS} PRIVATE gtest_main ${PROJECT_LIBS})

    include(GoogleTest)
    gtest_discover_tests(${PROJECT_TESTS})
endif()