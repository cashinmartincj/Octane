cmake_minimum_required(VERSION 3.20)
project(octane)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(octane
    main.cpp
    FileHandle.cpp
)

configure_file(index.html ${CMAKE_CURRENT_BINARY_DIR}/index.html COPYONLY)

target_include_directories(octane PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── asio header only — no find_package needed ──
target_compile_definitions(octane PRIVATE
    ASIO_STANDALONE          # no boost dependency
    _WIN32_WINNT=0x0601      # windows only — ignore on linux
)

# linux needs pthread
target_link_libraries(octane PRIVATE pthread)

# dev mode
option(OCTANE_DEV_MODE "Enable live reload" OFF)
if(OCTANE_DEV_MODE)
    target_compile_definitions(octane PRIVATE OCTANE_DEV_MODE)
endif()