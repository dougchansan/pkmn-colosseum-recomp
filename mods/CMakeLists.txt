cmake_minimum_required(VERSION 3.20)
project(PokemonColosseumModernGekkoMods VERSION 1.0.0 LANGUAGES C)

set(MODERNGEKKO_SOURCE_DIR "" CACHE PATH "ModernGekko source directory")
if(NOT IS_DIRECTORY "${MODERNGEKKO_SOURCE_DIR}/include/moderngekko")
    message(FATAL_ERROR "Set MODERNGEKKO_SOURCE_DIR to the ModernGekko checkout")
endif()

function(add_colosseum_feature_mod target mod_id display_name feature_export)
    add_library(${target} SHARED feature_mod.c)
    target_include_directories(${target} PRIVATE
        "${MODERNGEKKO_SOURCE_DIR}/include")
    target_compile_definitions(${target} PRIVATE
        MOD_ID="${mod_id}"
        MOD_DISPLAY_NAME="${display_name}"
        MOD_FEATURE_EXPORT="${feature_export}"
        MOD_VERSION="${PROJECT_VERSION}")
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        PREFIX ""
        OUTPUT_NAME "mod"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/${mod_id}.mgm"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/${mod_id}.mgm"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/${mod_id}.mgm")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/host-patches"
            "$<TARGET_FILE_DIR:${target}>/host-patches"
        COMMENT "Bundling reproducible ModernGekko/RecompCore host patches")
endfunction()

# Unlike the feature packages above, this one replaces guest code: native
# implementations of the paired-single matrix routines, which a wall-clock
# profile of guest PC showed to be the hottest code in the game at 60 FPS.
function(add_colosseum_patch_mod target mod_id display_name source)
    add_library(${target} SHARED ${source})
    target_include_directories(${target} PRIVATE
        "${MODERNGEKKO_SOURCE_DIR}/include")
    target_compile_definitions(${target} PRIVATE
        MOD_ID="${mod_id}"
        MOD_DISPLAY_NAME="${display_name}"
        MOD_VERSION="${PROJECT_VERSION}")
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        PREFIX ""
        OUTPUT_NAME "mod"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/${mod_id}.mgm"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/${mod_id}.mgm"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/${mod_id}.mgm")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/host-patches"
            "$<TARGET_FILE_DIR:${target}>/host-patches"
        COMMENT "Bundling reproducible ModernGekko/RecompCore host patches")
endfunction()

add_colosseum_patch_mod(
    colosseum_fastmath_mod
    pokemon-colosseum-fastmath
    "Pokemon Colosseum Native Matrix Math"
    fastmath_mod.c)
add_colosseum_patch_mod(
    colosseum_freecam_mod
    pokemon-colosseum-freecam
    "Pokemon Colosseum C-Stick Free Camera"
    freecam_mod.c)
# The first party Pokemon follows Wes in the field; Rui walks behind it.
add_colosseum_patch_mod(
    colosseum_follower_mod
    pokemon-colosseum-follower
    "Pokemon Colosseum Follower Pokemon"
    follower_mod.c)
# Capture-only: stages the attract-video remake (research/attract).
add_colosseum_patch_mod(
    colosseum_attract_stage_mod
    pokemon-colosseum-attract-stage
    "Pokemon Colosseum Attract Staging"
    attract_stage_mod.c)
# Colosseum Battle between two Colosseum saves (player 2 in the GBA slot).
add_colosseum_patch_mod(
    colosseum_online_battle_mod
    pokemon-colosseum-online-battle
    "Pokemon Colosseum Online Battle"
    online_battle_mod.c)
target_sources(colosseum_online_battle_mod PRIVATE online_battle_net.c online_battle_upnp.c)
if(WIN32)
    target_link_libraries(colosseum_online_battle_mod PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(colosseum_online_battle_mod PRIVATE Threads::Threads)
endif()
option(COLOSSEUM_FREECAM_TRACE "Log free-camera hook diagnostics" OFF)
if(COLOSSEUM_FREECAM_TRACE)
    target_compile_definitions(colosseum_freecam_mod PRIVATE FREECAM_TRACE=1)
endif()
add_custom_command(TARGET colosseum_freecam_mod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/menu-host"
        "$<TARGET_FILE_DIR:colosseum_freecam_mod>/menu-host"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/freecam-runtime.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/FREECAM.md"
        "$<TARGET_FILE_DIR:colosseum_freecam_mod>")
if(NOT WIN32)
    target_link_libraries(colosseum_freecam_mod PRIVATE m)
endif()
include(CTest)
if(BUILD_TESTING)
    add_executable(colosseum_freecam_test tests/freecam_test.c)
    target_include_directories(colosseum_freecam_test PRIVATE
        "${MODERNGEKKO_SOURCE_DIR}/include")
    set_target_properties(colosseum_freecam_test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES)
    if(NOT WIN32)
        target_link_libraries(colosseum_freecam_test PRIVATE m)
    endif()
    add_test(NAME colosseum_freecam COMMAND colosseum_freecam_test)
endif()
option(COLOSSEUM_FASTMATH_TRACE "Log that the native matrix patches are being reached" OFF)
if(COLOSSEUM_FASTMATH_TRACE)
    target_compile_definitions(colosseum_fastmath_mod PRIVATE FASTMATH_TRACE=1)
endif()

add_colosseum_feature_mod(
    colosseum_ultrawide_mod
    pokemon-colosseum-ultrawide
    "Pokemon Colosseum Native Ultrawide"
    moderngekko.feature.native_ultrawide)
add_colosseum_feature_mod(
    colosseum_hd_text_mod
    pokemon-colosseum-hd-text
    "Pokemon Colosseum HD Text"
    moderngekko.feature.text_upscale)
add_colosseum_feature_mod(
    colosseum_minimap_mod
    pokemon-colosseum-minimap
    "Pokemon Colosseum High-Contrast Minimap"
    moderngekko.feature.minimap_high_contrast)
# Research only: counts GX SDK calls and direct gather-pipe writers by caller
# (MODERNGEKKO_GX_ATTRIB=<path>). Target list: tools/gx-census/gx_targets.py.
add_colosseum_patch_mod(
    colosseum_gx_attrib_mod
    pokemon-colosseum-gx-attrib
    "Pokemon Colosseum GX Attribution Census"
    gx_attrib_mod.c)
# Research only: the synchronisation contract between the game and the GPU
# (MODERNGEKKO_GX_SYNC=<path>): interrupts, waits, CP/PE/VI/PI register access.
add_colosseum_patch_mod(
    colosseum_gx_sync_mod
    pokemon-colosseum-gx-sync
    "Pokemon Colosseum GX Sync Census"
    gx_sync_mod.c)

# Wes/Rui co-op. Built only on request: it patches moveLeader at 0x8012E7BC,
# which is that function's address in the co-op DOL. The release DOL has it at
# 0x8012E7B8 and its heroMoveMain still calls partnerMove, so the patch has
# nothing to act on there - and the hook list cannot even be generated, because
# no function starts at that address in the retail table.
option(COLOSSEUM_COOP_MOD "Build the Wes/Rui co-op mod (needs the co-op DOL)" OFF)
if(COLOSSEUM_COOP_MOD)
    add_colosseum_patch_mod(
        colosseum_coop_mod
        "pokemon-colosseum-coop"
        "Pokemon Colosseum Wes/Rui co-op"
        coop_mod.c)
endif()
