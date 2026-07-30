cmake_minimum_required(VERSION 3.20)
project(skywave VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_GUI    "Build the SDL2 / Dear ImGui GUI"        ON)
option(ENABLE_CRYPTO "Build libsodium-backed encryption"      OFF)
option(ENABLE_VOICE  "Build Codec2 live voice"                ON)
option(BUILD_TESTS   "Build unit and integration tests"       ON)
option(BUILD_TOOLS   "Build chansim / bertool / siggen tools" ON)
option(ENABLE_ASAN   "AddressSanitizer build"                 OFF)
option(ENABLE_UBSAN  "UndefinedBehaviorSanitizer build"       OFF)
option(ENABLE_TSAN   "ThreadSanitizer build"                  OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
if(ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

if(ENABLE_CRYPTO)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
endif()
if(ENABLE_VOICE)
  pkg_check_modules(CODEC2 IMPORTED_TARGET codec2)
  if(NOT CODEC2_FOUND)
    find_library(CODEC2_LIB codec2)
    if(NOT CODEC2_LIB)
      message(WARNING "Codec2 not found - voice disabled (ENABLE_VOICE=OFF)")
      set(ENABLE_VOICE OFF)
    endif()
  endif()
endif()
if(ENABLE_GUI)
  pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
  if(NOT SDL2_FOUND)
    message(WARNING "SDL2 not found - GUI disabled (ENABLE_GUI=OFF)")
    set(ENABLE_GUI OFF)
  endif()
endif()

# --- skywave core library (everything except main) ---------------------------
add_library(skywave_core STATIC
  src/core/log.cpp
  src/core/config.cpp
  src/dsp/fft.cpp
  src/dsp/fir.cpp
  src/dsp/updown.cpp
  src/dsp/chirp.cpp
  src/dsp/zc.cpp
  src/phy/framer.cpp
  src/phy/equalizer.cpp
  src/phy/receiver.cpp
  src/sim/chansim.cpp
  src/link/frame.cpp
  src/link/fsm.cpp
  src/link/handshake.cpp
  src/link/arq.cpp
  src/link/arbiter.cpp
  src/link/engine.cpp
  src/link/amc.cpp
  src/link/chanmem.cpp
  src/link/probe.cpp
  src/link/mask.cpp
  src/link/monitor.cpp
  src/tcp/server.cpp
  src/tcp/control.cpp
  src/fec/crc.cpp
  src/fec/ldpc.cpp
  src/fec/ldpc_tables.cpp
  src/io/wav.cpp
  src/io/audio.cpp
  src/worker.cpp
)
target_include_directories(skywave_core PUBLIC src)
target_link_libraries(skywave_core PUBLIC PkgConfig::PORTAUDIO PkgConfig::ZSTD)
target_compile_definitions(skywave_core PUBLIC
  SKW_VERSION="${PROJECT_VERSION}"
  $<$<BOOL:${ENABLE_CRYPTO}>:SKW_HAVE_CRYPTO>
  $<$<BOOL:${ENABLE_VOICE}>:SKW_HAVE_VOICE>
  $<$<BOOL:${ENABLE_GUI}>:SKW_HAVE_GUI>
)

# --- main executable ----------------------------------------------------------
add_executable(skywave src/main.cpp)
target_link_libraries(skywave PRIVATE skywave_core)

install(TARGETS skywave RUNTIME DESTINATION bin)
install(DIRECTORY docs/ DESTINATION share/doc/skywave)

# --- tools --------------------------------------------------------------------
if(BUILD_TOOLS)
  add_executable(bertool tools/bertool.cpp)
  target_link_libraries(bertool PRIVATE skywave_core)
endif()

# --- tests --------------------------------------------------------------------
if(BUILD_TESTS)
  enable_testing()
  function(skw_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE skywave_core)
    add_test(NAME ${name} COMMAND ${name})
  endfunction()
  skw_test(test_core)
  skw_test(test_config)
  skw_test(test_audio_rings)
  skw_test(test_wav)
  skw_test(test_dsp)
  skw_test(test_crc_whiten)
  skw_test(test_ldpc)
  skw_test(test_chirp)
  skw_test(test_zc)
  skw_test(test_phy_tx)
  skw_test(test_chansim)
  skw_test(test_phy_rx)
  skw_test(test_conformance)
  skw_test(test_frame)
  skw_test(test_fsm)
  skw_test(test_handshake)
  skw_test(test_arq)
  skw_test(test_arbiter)
  skw_test(test_tcp)
  skw_test(test_m2)
  skw_test(test_m3_ramp)
  skw_test(test_amc)
  skw_test(test_harq)
  skw_test(test_chanmem)
  skw_test(test_probe)
endif()
