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
