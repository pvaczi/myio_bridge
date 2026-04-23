cmake_minimum_required(VERSION 3.25)

# CMP0167: FindBoost modul eltávolítva CMake 4.x-ben → BoostConfig.cmake-t használjuk
cmake_policy(SET CMP0167 NEW)

project(myIO_bridge
    VERSION 1.0.0
    DESCRIPTION "myIO <-> Home Assistant MQTT bridge"
    LANGUAGES C CXX
)

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
        # Clang-en a Boost fejlécek sok figyelmeztetést adnak, ezeket elnyomjuk
        -Wno-deprecated-declarations
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
# Szükséges komponensek:
#   - Boost.Asio      (header-only, de system + thread kell hozzá)
#   - Boost.Beast     (header-only)
#   - Boost.JSON      (header-only, de boost_json.cpp-vel fordítjuk)
#   - Boost.MQTT5     (header-only)
#   - Boost.System    (linkelendő)
#   - Boost.Thread    (linkelendő, Asio igényli Windows-on Clang-gel)
#
# vcpkg esetén:
#   vcpkg install boost-asio boost-beast boost-json boost-system boost-thread boost-mqtt5
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake

find_package(Boost REQUIRED COMPONENTS system thread)

# Boost.JSON: a common/src/boost_json.cpp tartalmazza a src.hpp-t (header-only mód).
# BOOST_JSON_NO_LIB: ne próbálja auto-linkelni a boost_json.lib-et.
# BOOST_JSON_SOURCE: jelzi, hogy mi fordítjuk a forrást (nem DLL import).
add_compile_definitions(BOOST_JSON_NO_LIB)
add_compile_definitions(BOOST_JSON_SOURCE)
add_compile_definitions(BOOST_MQTT5_NO_LIB)
add_compile_definitions(BOOST_ASIO_NO_LIB)

# ── common (Static Library) ───────────────────────────────────────────────────
# Megosztott kód: HTTP kliens, JSON parser, típusok, interfészek
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

# ── bridge (Executable) ───────────────────────────────────────────────────────
# Fő MQTT bridge alkalmazás
add_executable(bridge
    src/bridge/main.cpp
    src/bridge/bridge.cpp
    src/bridge/config_reader.cpp
    src/bridge/mqtt5_publisher.cpp
    src/bridge/async_http_client.cpp
)

target_include_directories(bridge
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge
)

target_link_libraries(bridge
    PRIVATE
        common  # Örökli a common/include-ot és a Boost függőségeket
)

# ── discovery (Executable) ────────────────────────────────────────────────────
# Szervizprogram: lekérdezi a myIO szervereket és generálja a bridge.json-t
add_executable(discovery
    src/discovery/main.cpp
)

target_link_libraries(discovery
    PRIVATE
        common
)

# ── Windows-specifikus beállítások ────────────────────────────────────────────
if(WIN32)
    # Unicode karakterkészlet
    add_compile_definitions(UNICODE _UNICODE)

    # Winsock2 (Boost.Asio igényli Windows-on)
    # PUBLIC-ként adjuk a common-hoz, így mindenki örökli
    target_link_libraries(common PUBLIC ws2_32 mswsock)
endif()

# ── Linux/macOS-specifikus beállítások ────────────────────────────────────────
if(UNIX)
    # pthreads (Boost.Asio igényli)
    find_package(Threads REQUIRED)
    target_link_libraries(common PUBLIC Threads::Threads)
endif()

# ── Telepítési célok ──────────────────────────────────────────────────────────
install(TARGETS bridge discovery RUNTIME DESTINATION bin)
install(FILES
    data/bridge.json
    data/discovery_input.json
    DESTINATION share/myio_bridge
    OPTIONAL
)
