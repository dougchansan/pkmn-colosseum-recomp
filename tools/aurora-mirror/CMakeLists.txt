# aurora-mirror: Aurora drawing the game's GX stream beside Dolphin (see main.cpp).
#
#   cmake -S tools/aurora-mirror -B build/aurora-mirror -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/aurora-mirror --target aurora-mirror
#
# Builds GXRuntime (inside ModernGekko's Dolphin fork) with its Aurora backend,
# retail-GX frontend and gxcore render sink, and links the mirror against them.
cmake_minimum_required(VERSION 3.25)
project(aurora_mirror LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
# aurora-native links GXRuntime's static libraries into a shared object, which
# on Linux needs them position-independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(GXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/ModernGekko/vendor/dolphin/GXRuntime"
    CACHE PATH "GXRuntime source")
set(GXRUNTIME_ENABLE_AURORA ON CACHE BOOL "" FORCE)
set(GXRUNTIME_ENABLE_AURORA_RECOMP ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
option(COLOSSEUM_PORTABLE_DEPENDENCIES "Use static bundled Unix dependencies for distribution" OFF)
if(COLOSSEUM_PORTABLE_DEPENDENCIES AND UNIX)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
  set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
  set(AURORA_DAWN_PROVIDER package CACHE STRING "" FORCE)
  set(AURORA_DAWN_LINKAGE static CACHE STRING "" FORCE)
  # SQLite3: macOS runners can resolve an old framework header (Mono's) that
  # lacks sqlite3_changes64; the bundled amalgamation is the portable choice.
  foreach(_package IN ITEMS absl fmt PNG Freetype zstd SQLite3)
    set(CMAKE_DISABLE_FIND_PACKAGE_${_package} TRUE)
  endforeach()
  set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
  if(APPLE)
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
  endif()
endif()
add_subdirectory("${GXRUNTIME_DIR}" gxruntime EXCLUDE_FROM_ALL)
# The interpreter/SDK copy runs in the executable, outside the module's fault
# handler code range. Only the separately compiled game module uses fastmem.
target_compile_definitions(gxruntime PRIVATE DOLRECOMP_NO_FASTMEM=1)

add_executable(aurora-mirror main.cpp)
target_link_libraries(aurora-mirror PRIVATE GXRuntime::aurora)

# aurora-native.dll: the same renderer inside the runner's process
# (MODERNGEKKO_AURORA_NATIVE, see native.cpp). A DLL keeps Aurora's
# dependencies (Dawn, SDL, absl) apart from Dolphin's.
add_library(aurora-native SHARED native.cpp)
target_link_libraries(aurora-native PRIVATE GXRuntime::aurora)

# colosseum-standalone: the recompiled game on GXRuntime alone, no Dolphin
# (standalone.cpp), drawing through the same renderer as aurora-native.
add_executable(colosseum-standalone standalone.cpp audio_out.cpp ax_hle.cpp gekko_interp.cpp host_config.cpp host_ui.cpp
               memcard.cpp native.cpp native_text.cpp pad_input.cpp presentation.cpp)
target_link_libraries(colosseum-standalone PRIVATE GXRuntime::aurora ${AURORA_SDL3_TARGET})
target_compile_definitions(colosseum-standalone PRIVATE DOLRECOMP_NO_FASTMEM=1)

# Keep a standalone build runnable without the ModernGekko runner's directory
# on PATH. Dawn loads DXC dynamically, so TARGET_RUNTIME_DLLS alone is not enough.
include("${GXRUNTIME_DIR}/graphics/aurora/cmake/AuroraCopyRuntimeDLLs.cmake")
aurora_copy_runtime_dlls(colosseum-standalone)

enable_testing()
add_executable(colosseum-host-tests EXCLUDE_FROM_ALL
  tests/host_settings_test.cpp host_config.cpp presentation.cpp)
target_include_directories(colosseum-host-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${GXRUNTIME_DIR}/include")
target_link_libraries(colosseum-host-tests PRIVATE ${AURORA_SDL3_TARGET})
if(MSVC)
  target_compile_options(colosseum-host-tests PRIVATE /UNDEBUG)
else()
  target_compile_options(colosseum-host-tests PRIVATE -UNDEBUG)
endif()
aurora_copy_runtime_dlls(colosseum-host-tests)
add_test(NAME standalone-host-settings COMMAND ${CMAKE_COMMAND} -E env
  "STANDALONE_PREF_DIR=${CMAKE_CURRENT_BINARY_DIR}/host-settings-test"
  $<TARGET_FILE:colosseum-host-tests>)

add_executable(colosseum-pad-tests EXCLUDE_FROM_ALL tests/pad_input_test.cpp pad_input.cpp)
target_include_directories(colosseum-pad-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${GXRUNTIME_DIR}/include")
target_link_libraries(colosseum-pad-tests PRIVATE ${AURORA_SDL3_TARGET})
if(MSVC)
  target_compile_options(colosseum-pad-tests PRIVATE /fp:strict)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # The regression inspects fenv; unused SIMD lanes must not raise exceptions.
  target_compile_options(colosseum-pad-tests PRIVATE -ffp-exception-behavior=strict)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(colosseum-pad-tests PRIVATE -frounding-math -ftrapping-math)
endif()
aurora_copy_runtime_dlls(colosseum-pad-tests)
add_test(NAME standalone-gamepad COMMAND colosseum-pad-tests)
set_tests_properties(standalone-gamepad PROPERTIES SKIP_RETURN_CODE 77)

add_executable(colosseum-idle-tests EXCLUDE_FROM_ALL tests/idle_wait_test.cpp)
target_include_directories(colosseum-idle-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME standalone-idle-wait COMMAND colosseum-idle-tests)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  add_executable(colosseum-ram-tests EXCLUDE_FROM_ALL tests/linux_guest_ram_test.cpp)
  target_include_directories(colosseum-ram-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${GXRUNTIME_DIR}/include")
  # Alias-observation regressions must exercise optimization, including when
  # the rest of the host is built with debug settings.
  target_compile_options(colosseum-ram-tests PRIVATE -O3)
  add_test(NAME standalone-linux-ram COMMAND colosseum-ram-tests)
endif()

# Local module generation reuses the portable tooling without linking the
# ModernGekko runtime or building its Dolphin runner.
option(COLOSSEUM_BUILD_SETUP_TOOLS "Build local disc/module setup tools" ON)
if(COLOSSEUM_BUILD_SETUP_TOOLS)
  set(MODERNGEKKO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/ModernGekko")
  set(DOLRECOMP_ENABLE_LLVM OFF CACHE BOOL "" FORCE)
  # The DolRecomp the runtime vendors: it carries native substitutions (HD
  # text, SDK fast paths). lib/DolRecomp is the older upstream copy without them.
  add_subdirectory("${MODERNGEKKO_DIR}/vendor/dolphin/DolRecomp" dolrecomp-build EXCLUDE_FROM_ALL)
  add_executable(colosseum-module-tool
    "${MODERNGEKKO_DIR}/tools/moderngekko_port.cpp"
    "${MODERNGEKKO_DIR}/tools/module_direct_build.cpp"
    "${MODERNGEKKO_DIR}/src/runtime/game.cpp")
  target_include_directories(colosseum-module-tool PRIVATE "${MODERNGEKKO_DIR}/include")
  option(COLOSSEUM_SETUP_RELOCATABLE "Discover bundled module sources at runtime" OFF)
  if(COLOSSEUM_SETUP_RELOCATABLE)
    set(_module_source_dir "")
  else()
    set(_module_source_dir "${MODERNGEKKO_DIR}")
  endif()
  # The module tool caches generated code. Fingerprint its portable inputs so
  # a new host release cannot silently reuse a module built from older helpers.
  file(GLOB_RECURSE _module_inputs CONFIGURE_DEPENDS
    "${GXRUNTIME_DIR}/include/*.h" "${GXRUNTIME_DIR}/src/core/*.c"
    "${GXRUNTIME_DIR}/src/core/*.h")
  file(GLOB _template_inputs CONFIGURE_DEPENDS
    "${MODERNGEKKO_DIR}/vendor/dolphin/module-template/*.c"
    "${MODERNGEKKO_DIR}/vendor/dolphin/module-template/*.h"
    "${MODERNGEKKO_DIR}/vendor/dolphin/module-template/*.py")
  list(APPEND _module_inputs ${_template_inputs})
  list(APPEND _module_inputs
    "${MODERNGEKKO_DIR}/vendor/dolphin/module-template/CMakeLists.txt"
    "${MODERNGEKKO_DIR}/vendor/dolphin/module-template/module.exports"
    "${MODERNGEKKO_DIR}/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompABI.h")
  file(GLOB_RECURSE _recompiler_inputs CONFIGURE_DEPENDS
    "${MODERNGEKKO_DIR}/vendor/dolphin/DolRecomp/src/*.c"
    "${MODERNGEKKO_DIR}/vendor/dolphin/DolRecomp/src/*.h")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_module_inputs} ${_recompiler_inputs})
  foreach(_kind IN ITEMS module recompiler)
    set(_digest "")
    foreach(_input IN LISTS _${_kind}_inputs)
      file(SHA256 "${_input}" _hash)
      string(APPEND _digest "${_hash}")
    endforeach()
    string(SHA256 _${_kind}_revision "${_digest}")
  endforeach()
  target_compile_definitions(colosseum-module-tool PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN COLOSSEUM_STANDALONE_MODULE=1
    MODERNGEKKO_SOURCE_DIR="${_module_source_dir}"
    MODERNGEKKO_RECOMPCORE_REVISION="${_module_revision}"
    MODERNGEKKO_DOLRECOMP_REVISION="${_recompiler_revision}")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    target_compile_definitions(colosseum-module-tool PRIVATE COLOSSEUM_STANDALONE_FASTMEM=1)
  endif()
  add_custom_target(colosseum-setup-tools DEPENDS colosseum-module-tool dolrecomp)
  add_custom_command(TARGET colosseum-module-tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:dolrecomp>"
      "$<TARGET_FILE_DIR:colosseum-module-tool>"
    VERBATIM)
  add_dependencies(colosseum-module-tool dolrecomp)

endif()

# Unicode host I/O regression: generated disposable files only, no disc needed.
add_executable(colosseum-path-tests EXCLUDE_FROM_ALL
  tests/utf8_path_test.cpp host_config.cpp memcard.cpp pad_input.cpp)
target_include_directories(colosseum-path-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${GXRUNTIME_DIR}/include")
target_link_libraries(colosseum-path-tests PRIVATE ${AURORA_SDL3_TARGET})
if(MSVC)
  target_compile_options(colosseum-path-tests PRIVATE /UNDEBUG)
else()
  target_compile_options(colosseum-path-tests PRIVATE -UNDEBUG)
endif()
aurora_copy_runtime_dlls(colosseum-path-tests)
add_test(NAME standalone-utf8-paths COMMAND ${CMAKE_COMMAND} -E env
  "STANDALONE_PATH_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/utf8-path-test"
  $<TARGET_FILE:colosseum-path-tests>)
if(MINGW)
  target_link_options(colosseum-standalone PRIVATE -municode)
endif()

if(WIN32 AND MSVC)
  # Host dependencies and setup CLIs contain narrow Windows APIs too. Select
  # UTF-8 for these processes on Windows 10 1903+ without a system locale change.
  set(_utf8_manifest "${CMAKE_CURRENT_SOURCE_DIR}/windows-utf8.manifest")
  foreach(_utf8_target IN ITEMS colosseum-standalone colosseum-module-tool dolrecomp)
    if(TARGET ${_utf8_target})
      target_link_options(${_utf8_target} PRIVATE
        /MANIFEST:EMBED "/MANIFESTINPUT:${_utf8_manifest}")
      set_property(TARGET ${_utf8_target} APPEND PROPERTY LINK_DEPENDS "${_utf8_manifest}")
    endif()
  endforeach()
endif()
