cmake_minimum_required(VERSION 3.20)
project(octane VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Backend Selection ─────────────────────────
option(OCTANE_USE_IO_URING "Use native io_uring backend instead of Asio" ON)

if(OCTANE_USE_IO_URING)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(liburing REQUIRED liburing)
endif()

# Asio is compiled into every build as the Linux epoll runtime fallback.
include(FetchContent)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio
    GIT_TAG        asio-1-28-0
)
FetchContent_MakeAvailable(asio)

# ── library ───────────────────────────────────
add_library(octane_lib
    src/FileHandle.cpp
    src/utils/InputValidation.cpp
)

target_include_directories(octane_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(octane_lib SYSTEM PUBLIC $<BUILD_INTERFACE:${asio_SOURCE_DIR}/asio/include>)
target_compile_definitions(octane_lib PUBLIC ASIO_STANDALONE _WIN32_WINNT=0x0601)

if(OCTANE_USE_IO_URING)
    target_include_directories(octane_lib PUBLIC ${liburing_INCLUDE_DIRS})
    target_link_libraries(octane_lib PUBLIC ${liburing_LIBRARIES} pthread)
    target_compile_definitions(octane_lib PUBLIC OCTANE_USE_IO_URING)
endif()
target_link_libraries(octane_lib PUBLIC pthread)

# ── dev mode ──────────────────────────────────
option(OCTANE_DEV_MODE "Enable live reload" OFF)
if(OCTANE_DEV_MODE)
    target_compile_definitions(octane_lib PUBLIC OCTANE_DEV_MODE)
endif()

# ── compiler optimizations ───────────────────
option(OCTANE_NATIVE_ARCH "Enable -march=native -mtune=native for peak host performance" ON)
option(OCTANE_ENABLE_LTO "Enable Interprocedural / Link-Time Optimization (LTO)" ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    if(OCTANE_NATIVE_ARCH AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_compile_options(octane_lib PUBLIC -march=native -mtune=native)
    endif()
    if(OCTANE_ENABLE_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
        if(ipo_supported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        endif()
    endif()
endif()

# ── examples ──────────────────────────────────
option(OCTANE_BUILD_EXAMPLES "Build examples" ON)
if(OCTANE_BUILD_EXAMPLES)
    add_subdirectory(examples/01_hello_world)
    add_subdirectory(examples/02_rest_api)
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
    add_executable(octane_input_validation_tests tests/input_validation.cpp)
    target_link_libraries(octane_input_validation_tests PRIVATE octane_lib)
    add_test(NAME input_validation COMMAND octane_input_validation_tests)
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
    add_executable(octane_execution_queue_tests tests/execution_queue.cpp)
    target_link_libraries(octane_execution_queue_tests PRIVATE octane_lib)
    add_test(NAME execution_queue COMMAND octane_execution_queue_tests)
    add_executable(octane_server_options_tests tests/server_options.cpp)
    target_link_libraries(octane_server_options_tests PRIVATE octane_lib)
    add_test(NAME server_options COMMAND octane_server_options_tests)
    add_executable(octane_server_fixture tests/server_fixture.cpp)
    target_link_libraries(octane_server_fixture PRIVATE octane_lib)
    add_executable(octane_websocket_tests tests/websocket_tests.cpp)
    target_link_libraries(octane_websocket_tests PRIVATE octane_lib)
    add_test(NAME websocket_tests COMMAND octane_websocket_tests)
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
