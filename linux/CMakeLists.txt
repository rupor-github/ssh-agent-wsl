cmake_minimum_required(VERSION 3.7)
message(STATUS "Running cmake version ${CMAKE_VERSION}")

project(ssh-agent-wsl)

message(STATUS "Running cmake version ${CMAKE_VERSION}")
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "default to release build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/../cmake)

include_directories("${PROJECT_BINARY_DIR}")
include_directories("${PROJECT_SOURCE_DIR}")

set(DEST_DIR "${PROJECT_SOURCE_DIR}/../bin")
message(STATUS "Destination directory: ${DEST_DIR}")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -s -static")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -s -static")
endif()
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE")

set(SRCS main.c)

add_executable(ssh-agent-wsl ${SRCS})
install(TARGETS ssh-agent-wsl DESTINATION ${DEST_DIR} CONFIGURATIONS Release)
