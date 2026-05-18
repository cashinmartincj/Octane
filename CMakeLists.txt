cmake_minimum_required(VERSION 3.20)
project(octane VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── fetch asio ────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio
    GIT_TAG        asio-1-28-0
)
FetchContent_MakeAvailable(asio)

# ── library ───────────────────────────────────
add_library(octane_lib src/FileHandle.cpp)

target_include_directories(octane_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${asio_SOURCE_DIR}/asio/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(octane_lib PUBLIC
    ASIO_STANDALONE
    _WIN32_WINNT=0x0601
)

target_link_libraries(octane_lib PUBLIC pthread)

# ── dev mode ──────────────────────────────────
option(OCTANE_DEV_MODE "Enable live reload" OFF)
if(OCTANE_DEV_MODE)
    target_compile_definitions(octane_lib PUBLIC OCTANE_DEV_MODE)
endif()

# ── examples ──────────────────────────────────
option(OCTANE_BUILD_EXAMPLES "Build examples" ON)
if(OCTANE_BUILD_EXAMPLES)
    add_subdirectory(examples/01_hello_world)
    # add_subdirectory(examples/02_rest_api)
    # add_subdirectory(examples/03_static_files)
endif()

# ── install ───────────────────────────────────
install(TARGETS octane_lib
    EXPORT OctaneTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ DESTINATION include/octane)

install(EXPORT OctaneTargets
    FILE OctaneTargets.cmake
    NAMESPACE Octane::
    DESTINATION lib/cmake/octane
)