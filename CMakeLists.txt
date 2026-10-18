cmake_minimum_required(VERSION 3.20)
project(SuperMarioBros3Recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(NESRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/nesrecomp")
if(NOT EXISTS "${NESRECOMP_ROOT}/runner/cyc/project.cmake")
    message(FATAL_ERROR "nesrecomp submodule missing: run `git submodule update --init --recursive`")
endif()

# The ROM is never committed. Pass -DNESRECOMP_ROM=..., or drop it in the repo
# root as baserom.nes.
set(NESRECOMP_ROM "" CACHE FILEPATH "Super Mario Bros. 3 (USA) .nes ROM")
set(NESRECOMP_HOST_COMPILER "" CACHE FILEPATH "Prebuilt host NESRecomp (optional)")
if(NOT NESRECOMP_ROM)
    foreach(candidate
            "baserom.nes"
            "Super Mario Bros. 3 (USA).nes"
            "Super Mario Bros. 3 (USA) (Rev 1).nes"
            "Super Mario Bros. 3 (USA) (Rev A).nes")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${candidate}")
            set(NESRECOMP_ROM "${CMAKE_CURRENT_SOURCE_DIR}/${candidate}" CACHE FILEPATH "" FORCE)
            break()
        endif()
    endforeach()
endif()
if(NOT NESRECOMP_ROM)
    message(FATAL_ERROR "No SMB3 ROM found. Put it at ${CMAKE_CURRENT_SOURCE_DIR}/baserom.nes "
                        "or pass -DNESRECOMP_ROM=<path>.")
endif()

if(MINGW)
    # llvm-mingw: fold libc++/libunwind/winpthread in, so only SDL2.dll ships.
    add_link_options(-static)
endif()

include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")

# SMB3 is MMC3 (TSROM, mapper 4) with 8 KiB battery-less WRAM at $6000.
nesrecomp_add_cycle_game(SuperMarioBros3Recomp
    ROM "${NESRECOMP_ROM}"
    GAME_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/game.toml"
    RECOMPILER "${NESRECOMP_HOST_COMPILER}"
    REGION NTSC
    HOST_EXTRAS src/cycle_extras.c src/smb3_widescreen.c)
target_include_directories(SuperMarioBros3Recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
