cmake_minimum_required(VERSION 3.20)
project(octane VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Backend Selection ─────────────────────────
option(OCTANE_USE_IO_URING "Use native io_uring backend instead of Asio" ON)

if(OCTANE_USE_IO_URING)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(liburing REQUIRED liburing)
else()
    # ── fetch asio ────────────────────────────────
    include(FetchContent)
    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio
        GIT_TAG        asio-1-28-0
    )
    FetchContent_MakeAvailable(asio)
endif()

# ── library ───────────────────────────────────
add_library(octane_lib src/FileHandle.cpp)

target_include_directories(octane_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(OCTANE_USE_IO_URING)
    target_include_directories(octane_lib PUBLIC ${liburing_INCLUDE_DIRS})
    target_link_libraries(octane_lib PUBLIC ${liburing_LIBRARIES} pthread)
    target_compile_definitions(octane_lib PUBLIC OCTANE_USE_IO_URING)
else()
    target_include_directories(octane_lib PUBLIC $<BUILD_INTERFACE:${asio_SOURCE_DIR}/asio/include>)
    target_compile_definitions(octane_lib PUBLIC ASIO_STANDALONE _WIN32_WINNT=0x0601)
    target_link_libraries(octane_lib PUBLIC pthread)
endif()

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

include(CTest)
if(BUILD_TESTING)
    add_executable(octane_request_tests tests/request_dispatch.cpp)
    target_link_libraries(octane_request_tests PRIVATE octane_lib)
    add_test(NAME request_dispatch COMMAND octane_request_tests)
endif()

option(OCTANE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(OCTANE_SANITIZERS)
    target_compile_options(octane_lib PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(octane_lib PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
    add_executable(octane_parser_tests tests/http_parser.cpp)
    target_link_libraries(octane_parser_tests PRIVATE octane_lib)
    add_test(NAME http_parser COMMAND octane_parser_tests)
    add_executable(octane_server_fixture tests/server_fixture.cpp)
    target_link_libraries(octane_server_fixture PRIVATE octane_lib)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME tcp_transport COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/transport_test.py $<TARGET_FILE:octane_server_fixture>)
    set_tests_properties(tcp_transport PROPERTIES TIMEOUT 20)
endif()

option(OCTANE_FUZZING "Build Clang libFuzzer HTTP parser target" OFF)
if(OCTANE_FUZZING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "OCTANE_FUZZING requires Clang")
    endif()
    add_executable(octane_fuzz_parser tests/fuzz_parser.cpp)
    target_link_libraries(octane_fuzz_parser PRIVATE octane_lib)
    target_compile_options(octane_fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(octane_fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(BUILD_TESTING)
    add_test(NAME benchmark_smoke COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_smoke.py $<TARGET_FILE:octane_server_fixture>)
    set_tests_properties(benchmark_smoke PROPERTIES TIMEOUT 75 LABELS benchmark)
endif()