# Compiler: pick it when configuring, e.g. CXX=clang++ cmake -B build
cmake_minimum_required(VERSION 3.20)

set(project_name oticon_connect)
project(${project_name} C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SPANDSP REQUIRED IMPORTED_TARGET spandsp)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

include(cmake/btstack.cmake)

set(OTICON_SANITIZER "thread" CACHE STRING "Sanitizer: none, thread (ring test), address-undefined")
set_property(CACHE OTICON_SANITIZER PROPERTY STRINGS none thread address-undefined)
if(NOT OTICON_SANITIZER MATCHES "^(none|thread|address-undefined)$")
  message(FATAL_ERROR "Unknown OTICON_SANITIZER: ${OTICON_SANITIZER}")
endif()

# Warnings for our code only (BTstack is built as is)
function(project_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  if(OTICON_SANITIZER STREQUAL "address-undefined")
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

# Includes are relative to src/: #include "ble/ble_link.h"
set(SRC ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Libraries, one per directory of src/
# ---------------------------------------------------------------------------

# audio/: G.722 encoder, frame sources (file, tone); no Bluetooth
add_library(audio STATIC
    src/audio/frame_source.cc
    src/audio/g722_encode.cc
)
target_include_directories(audio PUBLIC ${SRC})
target_link_libraries(audio PRIVATE PkgConfig::SPANDSP)
project_warnings(audio)

# audio/: PipeWire sink "Oticon ASHA" and the frame source on top of it
add_library(audio_pipewire STATIC
    src/audio/pipewire_source.cc
    src/audio/pw_sink.cc
)
target_link_libraries(audio_pipewire PUBLIC audio PRIVATE PkgConfig::PIPEWIRE)
project_warnings(audio_pipewire)

# ble/: BLE link over BTstack, BlueZ bond parser, bonds in the device DB, run loop timer
add_library(ble STATIC
    src/ble/ble_link.cc
    src/ble/bond_parser.cc
    src/ble/bond_store.cc
    src/ble/device_picker.cc
)
target_include_directories(ble PUBLIC ${SRC})
target_link_libraries(ble PUBLIC btstack)
project_warnings(ble)

# asha/: ASHA protocol and the 20 ms audio stream over the link
add_library(asha STATIC
    src/asha/asha_session.cc
    src/asha/audio_stream.cc
)
target_link_libraries(asha PUBLIC ble audio)
project_warnings(asha)

# ---------------------------------------------------------------------------
# Programs
# ---------------------------------------------------------------------------

# app/: the client: options, signals, BTstack init (main.cc) + glue (asha_probe.cc)
add_executable(${project_name}
    src/app/asha_probe.cc
    src/app/main.cc
    src/app/options.cc
    src/app/usb_adapters.cc
    src/app/wifi_avoidance.cc
)
target_link_libraries(${project_name} asha audio_pipewire)
project_warnings(${project_name})

# tools/: sine -> G.722 file
add_executable(g722_gen tools/g722_gen.cc)
target_link_libraries(g722_gen audio)
project_warnings(g722_gen)

# tools/: PipeWire sink -> raw file, no Bluetooth
add_executable(pw_capture tools/pw_capture.cc)
target_link_libraries(pw_capture audio_pipewire)
project_warnings(pw_capture)

# ---------------------------------------------------------------------------
# Tests: ctest --test-dir build
# ---------------------------------------------------------------------------

enable_testing()

# SPSC ring under two threads; ThreadSanitizer catches wrong memory orders
add_executable(ring_test tests/ring_test.cc)
target_include_directories(ring_test PRIVATE ${SRC})
if(OTICON_SANITIZER STREQUAL "thread")
  target_compile_options(ring_test PRIVATE -fsanitize=thread -g)
  target_link_options(ring_test PRIVATE -fsanitize=thread)
endif()
project_warnings(ring_test)
add_test(NAME ring_test COMMAND ring_test)

add_executable(g722_reset_test tests/g722_reset_test.cc)
target_link_libraries(g722_reset_test PRIVATE audio)
project_warnings(g722_reset_test)
add_test(NAME g722_reset_test COMMAND g722_reset_test)

add_executable(wifi_avoidance_test
    tests/wifi_avoidance_test.cc
    src/app/wifi_avoidance.cc
)
target_include_directories(wifi_avoidance_test PRIVATE ${SRC})
project_warnings(wifi_avoidance_test)
add_test(NAME wifi_avoidance_test COMMAND wifi_avoidance_test)

add_executable(options_test tests/options_test.cc src/app/options.cc)
target_include_directories(options_test PRIVATE ${SRC})
project_warnings(options_test)
add_test(NAME options_test COMMAND options_test)

add_executable(pw_sink_lifetime_test tests/pw_sink_lifetime_test.cc src/audio/pw_sink.cc)
target_include_directories(pw_sink_lifetime_test PRIVATE ${SRC})
target_link_libraries(pw_sink_lifetime_test PRIVATE PkgConfig::PIPEWIRE)
project_warnings(pw_sink_lifetime_test)
add_test(NAME pw_sink_lifetime_test COMMAND pw_sink_lifetime_test)
