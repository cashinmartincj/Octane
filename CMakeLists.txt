cmake_minimum_required(VERSION 3.20)
project(octane CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(asio QUIET)

if(NOT asio_FOUND)
    include(FetchContent)
    FetchContent_Declare(asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG asio-1-30-2
    )
    FetchContent_MakeAvailable(asio)
    set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
endif()

add_executable(octane main.cpp FileHandle.cpp)

option(OCTANE_DEV "Enable dev mode" OFF)

if(OCTANE_DEV)
    target_compile_definitions(octane PRIVATE OCTANE_DEV_MODE)
endif()

file(COPY ${CMAKE_SOURCE_DIR}/index.html DESTINATION ${CMAKE_BINARY_DIR})

if(ASIO_INCLUDE_DIR)
    target_include_directories(octane PRIVATE ${ASIO_INCLUDE_DIR})
endif()

target_compile_definitions(octane PRIVATE ASIO_STANDALONE)