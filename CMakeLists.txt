cmake_minimum_required(VERSION 3.10.0)
project(nats-connector)

# include nats.c library into our project - doing it by automatically fetching latest stable version from GitHub
#set(NATS_BUILD_STREAMING OFF CACHE BOOL "Disable NATS Streaming build")
#include(FetchContent)
#FetchContent_Declare(
#  nats
#  GIT_REPOSITORY https://github.com/nats-io/nats.c
#  GIT_TAG v3.10.1  # latest stable tag on 04/08/2025
#)
#FetchContent_MakeAvailable(nats)

find_package(cnats CONFIG REQUIRED)
# initial version nats.c library used in this project is 3.10.1

set(PROJECT_SOURCES
    src/main.cpp
    src/NatsPublisher.cpp
    src/NatsSubscriber.cpp
)

add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(${PROJECT_NAME} PUBLIC cnats::nats)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)