cmake_minimum_required(VERSION 3.16)
project(pakon C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wshadow)

# --- libusb-1.0 via pkg-config -------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

# --- libpakon: transport + protocol + log --------------------------------
add_library(pakon STATIC
  src/pakon_log.c
  src/pakon_proto.c
  src/pakon_hex.c
  src/pakon_cmd.c
  src/pakon_usb.c
  src/pakon_calib.c
  src/pakon_eeprom.c
  src/pakon_setup.c
)
target_include_directories(pakon PUBLIC ${CMAKE_SOURCE_DIR}/include)
# libusb is an implementation detail of the transport layer, but a static
# archive propagates its link deps; PkgConfig::LIBUSB carries the search dirs
# so downstream targets (incl. test_proto) link cleanly.
target_link_libraries(pakon PRIVATE PkgConfig::LIBUSB)
# pakon_calib uses libm (gain linearization); expose it on the interface so the
# tools and tests that link the static archive resolve the math symbols.
target_link_libraries(pakon PUBLIC m)

# --- tools (standalone) ---------------------------------------------------
add_executable(pakon_probe tools/pakon_probe.c)
target_link_libraries(pakon_probe PRIVATE pakon)

add_executable(pakon_replay tools/pakon_replay.c)
target_link_libraries(pakon_replay PRIVATE pakon)

# --- tests (no hardware) --------------------------------------------------
enable_testing()
add_executable(test_proto test/test_proto.c)
target_link_libraries(test_proto PRIVATE pakon)
add_test(NAME test_proto COMMAND test_proto)

add_executable(test_hex test/test_hex.c)
target_link_libraries(test_hex PRIVATE pakon)
add_test(NAME test_hex COMMAND test_hex)

add_executable(test_calib test/test_calib.c)
target_link_libraries(test_calib PRIVATE pakon)
add_test(NAME test_calib COMMAND test_calib)

add_executable(test_eeprom_c test/test_eeprom_c.c)
target_link_libraries(test_eeprom_c PRIVATE pakon)
add_test(NAME test_eeprom_c
  COMMAND test_eeprom_c ${CMAKE_SOURCE_DIR}/test/fixtures/eeprom)

add_executable(test_setup test/test_setup.c)
target_link_libraries(test_setup PRIVATE pakon)
add_test(NAME test_setup COMMAND test_setup ${CMAKE_SOURCE_DIR}/resources)

# EEPROM decoding and mocked read-only transfers require only Python stdlib.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME test_eeprom
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_eeprom.py)
else()
  message(STATUS "Python3 not found: offline EEPROM tests are unavailable")
endif()
