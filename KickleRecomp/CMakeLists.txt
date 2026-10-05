cmake_minimum_required(VERSION 3.20)
project(KickleCubicleNESRecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(NESRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../nesrecomp" CACHE PATH
    "Path to the NESRecomp source tree")
option(NESRECOMP_USE_CYCLE_BACKEND "Use cycle accurate native code" ON)
option(KICKLE_DEVELOPMENT_ROM "Preselect the developer's local ROM on launch" OFF)
if(NESRECOMP_USE_CYCLE_BACKEND)
    set(NESRECOMP_RECOMP_UI "${CMAKE_CURRENT_SOURCE_DIR}/../ZeldaTemplate/recomp-ui" CACHE STRING "Shared Zelda frontend")
    include(${NESRECOMP_ROOT}/runner/cyc/project.cmake)
    set(KICKLE_ROM "${CMAKE_CURRENT_SOURCE_DIR}/../roms/Kickle Cubicle (USA).nes" CACHE FILEPATH "Local original USA ROM (not distributed)")
    set(KICKLE_GALE_ROM "${CMAKE_CURRENT_SOURCE_DIR}/../roms/Kickle Cubicle Gale Festival.nes" CACHE FILEPATH "Local Gale Festival patched ROM (not distributed)")
    if(NOT EXISTS "${KICKLE_ROM}" OR NOT EXISTS "${KICKLE_GALE_ROM}")
        message(FATAL_ERROR "Supply your own ROMs using -DKICKLE_ROM=... and -DKICKLE_GALE_ROM=...; no ROMs are included.")
    endif()
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_gale.py"
        "${KICKLE_ROM}" "${KICKLE_GALE_ROM}" "${CMAKE_CURRENT_BINARY_DIR}/gale_data.h"
        RESULT_VARIABLE gale_result)
    if(NOT gale_result EQUAL 0)
        message(FATAL_ERROR "Could not generate local Gale Festival data")
    endif()
    nesrecomp_add_cycle_game(KickleCubicleNESRecomp
        ROM "${KICKLE_ROM}"
        GAME_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/cycle.toml"
        PLAYERS 1 HOST_EXTRAS "${CMAKE_CURRENT_SOURCE_DIR}/kickle_widescreen.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/kickle_voxel.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/kickle_voxel_renderer.c")
    target_include_directories(KickleCubicleNESRecomp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(KickleCubicleNESRecomp PRIVATE
        CYC_KICKLE_BRANDING
        CYC_CROP_VERTICAL_OVERSCAN)
    if(KICKLE_DEVELOPMENT_ROM)
        target_include_directories(KickleCubicleNESRecomp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(KickleCubicleNESRecomp PRIVATE
            "CYC_DEFAULT_ROM=\"${KICKLE_ROM}\"")
    endif()
    if(WIN32)
        enable_language(RC)
        target_sources(KickleCubicleNESRecomp PRIVATE kickle.rc)
    endif()
    add_custom_command(TARGET KickleCubicleNESRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/kickle"
        "$<TARGET_FILE_DIR:KickleCubicleNESRecomp>/assets/kickle")
    add_custom_command(TARGET KickleCubicleNESRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/default-config.ini" "$<TARGET_FILE_DIR:KickleCubicleNESRecomp>/config.ini")
    return()
endif()
message(FATAL_ERROR "This source package requires NESRECOMP_USE_CYCLE_BACKEND=ON.")

