cmake_minimum_required(VERSION 3.25)

# CMP0167: FindBoost module removed in CMake 4.x — use BoostConfig.cmake instead
cmake_policy(SET CMP0167 NEW)

# NOTE: the project VERSION below is a fixed placeholder (like vcpkg.json's
# version-string) — the real release version is the git tag, injected by CI
# as BRIDGE_VERSION and reported by `bridge --version`.
project(myIO_bridge
    VERSION 1.0.0
    DESCRIPTION "myIO <-> Home Assistant MQTT bridge"
    LANGUAGES C CXX
)

# ── Version ───────────────────────────────────────────────────────────────────
# CI injects the git tag via -DBRIDGE_VERSION=${github.ref_name}; "dev" locally.
if(NOT DEFINED BRIDGE_VERSION)
    set(BRIDGE_VERSION "dev")
endif()

# ── C++23 ─────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler warnings ─────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        -Wno-deprecated-declarations  # Boost headers trigger many on Clang
    )
elseif(MSVC)
    add_compile_options(
        /W4
        /w44365          # signed/unsigned mismatch
        /bigobj          # large object files (Boost headers)
        /permissive-     # conformance mode
        /utf-8
    )
endif()

# ── Boost ─────────────────────────────────────────────────────────────────────
# Required components:
#   - Boost.Asio      (header-only, requires system + thread)
#   - Boost.Beast     (header-only)
#   - Boost.JSON      (header-only, compiled via boost_json.cpp)
#   - Boost.MQTT5     (header-only)
#   - Boost.System    (linked)
#   - Boost.Thread    (linked; required by Asio on Windows with Clang)
#
# With vcpkg:
#   vcpkg install boost-asio boost-beast boost-json boost-system boost-thread boost-mqtt5
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake

find_package(Boost REQUIRED COMPONENTS system thread)

# Boost.JSON: src/common/boost_json.cpp includes src.hpp (single compilation unit).
# BOOST_JSON_NO_LIB: prevent auto-linking boost_json.lib.
# BOOST_JSON_SOURCE: signals that we compile the source (not a DLL import).
add_compile_definitions(BOOST_JSON_NO_LIB)
add_compile_definitions(BOOST_JSON_SOURCE)
add_compile_definitions(BOOST_MQTT5_NO_LIB)
add_compile_definitions(BOOST_ASIO_NO_LIB)

# ── common (Static Library) ───────────────────────────────────────────────────
# Shared code: HTTP client, JSON parser, types, interfaces
add_library(common STATIC
    src/common/boost_json.cpp
    src/common/dsens_parser.cpp
    src/common/http_client.cpp
)

target_include_directories(common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
        ${Boost_INCLUDE_DIRS}
)

target_link_libraries(common
    PUBLIC
        Boost::system
        Boost::thread
)

# ── bridge_config (Static Library) ────────────────────────────────────────────
# bridge.json reader + pure bridge rules, shared by the bridge executable and the unit tests
add_library(bridge_config STATIC
    src/bridge/config_reader.cpp
    src/bridge/bridge_rules.cpp
)

target_include_directories(bridge_config
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge
)

target_link_libraries(bridge_config
    PUBLIC
        common
)

# ── bridge (Executable) ───────────────────────────────────────────────────────
# Main MQTT bridge application
add_executable(bridge
    src/bridge/main.cpp
    src/bridge/bridge.cpp
    src/bridge/mqtt5_publisher.cpp
    src/bridge/async_http_client.cpp
)

target_include_directories(bridge
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge
)

target_link_libraries(bridge
    PRIVATE
        bridge_config
        common  # inherits common/include and Boost dependencies
)

target_compile_definitions(bridge PRIVATE BRIDGE_VERSION="${BRIDGE_VERSION}")

# ── discovery (Executable) ────────────────────────────────────────────────────
# Service tool: queries myIO servers and generates bridge.json
add_executable(discovery
    src/discovery/main.cpp
)

target_link_libraries(discovery
    PRIVATE
        common
)

# ── bridge_tests (Unit tests) ─────────────────────────────────────────────────
# Dependency-free test executable; run with: ctest --test-dir build
enable_testing()

add_executable(bridge_tests
    tests/bridge_tests.cpp
)

target_link_libraries(bridge_tests
    PRIVATE
        bridge_config
)

add_test(NAME bridge_tests COMMAND bridge_tests)

# ── Windows-specific settings ─────────────────────────────────────────────────
if(WIN32)
    # Unicode character set
    add_compile_definitions(UNICODE _UNICODE)

    # Winsock2 (required by Boost.Asio on Windows)
    # Added as PUBLIC so all dependents inherit it
    target_link_libraries(common PUBLIC ws2_32 mswsock)
endif()

# ── Linux/macOS-specific settings ─────────────────────────────────────────────
if(UNIX)
    # pthreads (required by Boost.Asio)
    find_package(Threads REQUIRED)
    target_link_libraries(common PUBLIC Threads::Threads)
endif()

# ── Install targets ───────────────────────────────────────────────────────────
install(TARGETS bridge discovery RUNTIME DESTINATION bin)
install(FILES
    data/bridge.json
    data/discovery_input.json
    DESTINATION share/myio_bridge
    OPTIONAL
)
