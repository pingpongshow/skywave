cmake_minimum_required(VERSION 3.20)
project(skywave VERSION 1.2.6 LANGUAGES C CXX)

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
option(BUILD_TOOLS   "Build bertool / fmsweep / fuzztool / on-air tools" ON)
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
set(SKW_CRYPTO_SOURCES)
if(ENABLE_CRYPTO)
  list(APPEND SKW_CRYPTO_SOURCES src/crypto/session.cpp)
endif()
if(ENABLE_VOICE)
  list(APPEND SKW_CRYPTO_SOURCES src/voice/codec.cpp src/voice/session.cpp)
endif()
if(ENABLE_GUI)
  # Dear ImGui (vendored, docking branch) + SDL2 renderer backend.
  set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
  list(APPEND SKW_CRYPTO_SOURCES
    src/gui/gui.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
  )
endif()

add_library(skywave_core STATIC
  ${SKW_CRYPTO_SOURCES}
  src/core/log.cpp
  src/core/config.cpp
  src/dsp/fft.cpp
  src/dsp/fir.cpp
  src/dsp/updown.cpp
  src/dsp/chirp.cpp
  src/dsp/zc.cpp
  src/dsp/morse.cpp
  src/dsp/css.cpp
  src/phy/framer.cpp
  src/phy/equalizer.cpp
  src/phy/receiver.cpp
  src/sim/chansim.cpp
  src/sim/radiochain.cpp
  src/sim/fmradio.cpp
  src/link/frame.cpp
  src/link/fsm.cpp
  src/link/handshake.cpp
  src/link/arq.cpp
  src/link/advisor.cpp
  src/link/engine.cpp
  src/link/amc.cpp
  src/link/chanclass.cpp
  src/link/chanmem.cpp
  src/link/probe.cpp
  src/link/mask.cpp
  src/link/monitor.cpp
  src/proto/pipeline.cpp
  src/proto/filesvc.cpp
  src/tcp/server.cpp
  src/tcp/control.cpp
  src/fec/crc.cpp
  src/fec/ldpc.cpp
  src/fec/ldpc_tables.cpp
  src/io/wav.cpp
  src/io/audio.cpp
  src/io/ptt.cpp
  src/worker.cpp
)
target_include_directories(skywave_core PUBLIC src)
target_link_libraries(skywave_core PUBLIC PkgConfig::PORTAUDIO PkgConfig::ZSTD)
if(ENABLE_CRYPTO)
  target_link_libraries(skywave_core PUBLIC PkgConfig::SODIUM)
endif()
if(ENABLE_GUI)
  target_link_libraries(skywave_core PUBLIC PkgConfig::SDL2)
  target_include_directories(skywave_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
  # Vendored third-party code is not ours to lint.
  set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp
    PROPERTIES COMPILE_OPTIONS "-w")
endif()
if(ENABLE_VOICE)
  if(CODEC2_FOUND)
    target_link_libraries(skywave_core PUBLIC PkgConfig::CODEC2)
    # codec2.h includes <codec2/version.h>, so the PARENT of the
    # pkg-config include dir must be on the path as well.
    foreach(d ${CODEC2_INCLUDE_DIRS})
      get_filename_component(_parent "${d}" DIRECTORY)
      target_include_directories(skywave_core PUBLIC "${_parent}")
    endforeach()
  else()
    target_link_libraries(skywave_core PUBLIC ${CODEC2_LIB})
  endif()
endif()
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
install(FILES THIRD_PARTY.md DESTINATION share/doc/skywave)

# --- tools --------------------------------------------------------------------
if(BUILD_TOOLS)
  add_executable(bertool tools/bertool.cpp)
  target_link_libraries(bertool PRIVATE skywave_core)
  add_executable(fuzztool tools/fuzztool.cpp)
  target_link_libraries(fuzztool PRIVATE skywave_core)
  add_executable(fmsweep tools/fmsweep.cpp)
  target_link_libraries(fmsweep PRIVATE skywave_core)
  # On-air test tooling (docs/ONAIR_TEST_PLAN.md): kept out of the modem.
  add_executable(onair_sounder tools/onair/sounder.cpp)
  target_link_libraries(onair_sounder PRIVATE skywave_core)
endif()

# --- tests --------------------------------------------------------------------
if(BUILD_TESTS)
  enable_testing()
  function(skw_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE skywave_core)
    add_test(NAME ${name} COMMAND ${name})
    # Tests must never touch the operator's real channel-memory store.
    set_tests_properties(${name} PROPERTIES
      ENVIRONMENT "SKW_CHANMEM_PATH=")
  endfunction()
  skw_test(test_core)
  skw_test(test_config)
  skw_test(test_audio_rings)
  skw_test(test_wav)
  skw_test(test_dsp)
  skw_test(test_css)
  skw_test(test_css_phy)
  skw_test(test_css_link)
  skw_test(test_vhf)
  skw_test(test_fmradio)
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
  skw_test(test_tcp)
  skw_test(test_m2)
  skw_test(test_m3_ramp)
  skw_test(test_system)
  skw_test(test_proto)
  skw_test(test_file)
  skw_test(test_bcast)
  # test_st2 runs over the HALF-DUPLEX world by default: each station
  # deaf while its own PTT is keyed and for the TX/RX switching interval
  # after (tests/st2_world.hpp) - the radio this modem is built for. The
  # old split-TX/RX world never punished keying over the peer and cannot
  # judge a scheduler; it is kept as the full-duplex VARIANT below.
  skw_test(test_st2)
  add_test(NAME test_st2_fullduplex COMMAND test_st2)
  set_tests_properties(test_st2_fullduplex PROPERTIES
    ENVIRONMENT "SKW_CHANMEM_PATH=;SKW_FULL_DUPLEX=1")
  # Long-run endurance scenarios (soak_st2): built always, NOT registered
  # with ctest (each scenario is 900 sim-s, minutes of wall clock, and
  # churn/floor are still gated on the decode-latency lever - see
  # ROADMAP). Run manually: SKW_SOAK_SCENARIO={churn,floor,blackout}. The
  # world is half-duplex by default (SKW_FULL_DUPLEX=1 for the old split
  # world) and SKW_SOAK_COUPLED=1 reproduces the historical schedule.
  # Scripted fault sessions over the same world are the registered
  # test_st2 (+ its full-duplex variant).
  add_executable(soak_st2 tests/soak_st2.cpp)
  target_link_libraries(soak_st2 PRIVATE skywave_core)
  # CFO step / offset / drift scenarios over ST2 (GAP_CLOSURE_PLAN 8.0,
  # gates 8.2/8.6/8.7): one-way bulk for 200 sim-s must never drop and
  # must deliver >= ~85% of the no-offset volume (~65 KB).
  add_executable(cfo_st2 tests/cfo_st2.cpp)
  target_link_libraries(cfo_st2 PRIVATE skywave_core)
  # Deterministic soak cycle-rate harness (not registered).
  add_executable(cycle_st2 tests/cycle_st2.cpp)
  target_link_libraries(cycle_st2 PRIVATE skywave_core)
  function(skw_cfo_st2 name scenario)
    add_test(NAME cfo_st2_${name} COMMAND cfo_st2)
    set_tests_properties(cfo_st2_${name} PROPERTIES
      ENVIRONMENT "SKW_CHANMEM_PATH=;DUR=200;MIN_BYTES=55000;${scenario}"
      TIMEOUT 3000)
  endfunction()
  skw_cfo_st2(step40 "STEP=40;REV_MAX=5")
  skw_cfo_st2(cfo45 "CFO0=45")
  skw_cfo_st2(drift01 "DRIFT=0.1")
  add_executable(monkey_st2 tests/monkey_st2.cpp)
  target_link_libraries(monkey_st2 PRIVATE skywave_core)
  # Minimal one-way bulk repro (diagnostic, not a registered test).
  add_executable(repro_oneway tests/repro_oneway.cpp)
  target_link_libraries(repro_oneway PRIVATE skywave_core)
  if(ENABLE_CRYPTO)
    skw_test(test_crypto)
  endif()
  if(ENABLE_VOICE)
    skw_test(test_voice)
  endif()
  if(ENABLE_GUI)
    skw_test(test_gui)
  endif()
  # ST3 (TESTING.md): two real processes over real/virtual audio devices.
  # Labeled `hardware` and excluded from default runs: ctest -L hardware.
  option(SKW_HARDWARE_TESTS "Enable the ST3 real-audio-device gate" OFF)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    # Release gate 7 in prose form: the floor tables published in
    # WAVEFORM.md must equal kFloors. Three NARROW entries had drifted
    # without anything noticing, because a stale number in a markdown
    # table is indistinguishable from a fresh one. Cheap, no build deps.
    add_test(NAME docs_floors
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_docs_floors.py)
    set_tests_properties(docs_floors PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME st3_hardware
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/st3_hardware.py
                     --binary $<TARGET_FILE:skywave>)
    # Gated by a CACHE OPTION, not $ENV{...}: the generator expression is
    # evaluated when CMAKE CONFIGURES, so the previously documented
    # `SKW_HARDWARE_TESTS=1 ctest -L hardware` silently ran NOTHING on a
    # tree configured without the variable set - "Not Run (Disabled)",
    # then "No tests were found!!!", and ctest still exited 0, so the
    # release gate could be "run" without ever executing. An option is
    # explicit, survives in the cache, and shows up in the build config.
    # The script still exits 77 (SKIP_RETURN_CODE) when the loopback
    # devices are absent, so enabling it on a machine without them
    # SKIPS rather than fails.
    # RESOURCE_LOCK: both hardware tests drive the SAME pair of loopback
    # audio devices, so ctest must never schedule them together. Without
    # it a `ctest -j` (or a stray manual run) makes one of them fail in a
    # couple of seconds on device-open, which looks like a modem defect
    # and is not one.
    # SKW_CHANMEM_PATH= for the same reason skw_test() sets it: a test
    # must never touch the operator's real channel-memory store. These
    # two were missed, so every ST3 run has been reading AND WRITING
    # ~/.skywave/chanmem.bin - which also feeds the initial-mode seed on
    # each reconnect, making the kill/resume gate depend on whatever the
    # last run happened to leave behind.
    set_tests_properties(st3_hardware PROPERTIES
      LABELS "hardware"
      TIMEOUT 14400
      SKIP_RETURN_CODE 77
      RESOURCE_LOCK "loopback_audio"
      ENVIRONMENT "SKW_CHANMEM_PATH="
      DISABLED $<NOT:$<BOOL:${SKW_HARDWARE_TESTS}>>)
    # SIGKILL/resume hammer over the same devices. The script existed but
    # was registered with NOTHING, so no gate ever ran it; it also guards
    # itself on SKW_HARDWARE_TESTS=1 in the ENVIRONMENT, which the option
    # above does not set, so even a manual run printed "skip" and exited
    # 0. Registered here with the label, the option, and the environment
    # its own guard expects.
    add_test(NAME st3_killresume
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/st3_killresume.py
                     --binary $<TARGET_FILE:skywave>)
    set_tests_properties(st3_killresume PROPERTIES
      LABELS "hardware"
      TIMEOUT 6000
      SKIP_RETURN_CODE 77
      RESOURCE_LOCK "loopback_audio"
      ENVIRONMENT "SKW_HARDWARE_TESTS=1;SKW_CHANMEM_PATH="
      DISABLED $<NOT:$<BOOL:${SKW_HARDWARE_TESTS}>>)
  endif()
  skw_test(test_ptt)
  skw_test(test_monitor_worker)
  skw_test(test_amc)
  skw_test(test_advisor)
  skw_test(test_harq)
  skw_test(test_chanclass)
  skw_test(test_chanmem)
  skw_test(test_probe)
endif()
