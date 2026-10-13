cmake_minimum_required(VERSION 3.20)
project(TwistedMetal1234Recomp LANGUAGES C)

# Each game is a separate executable: the recompiled game code is linked into
# it, and the framework's build expects one game per CMake project. This
# top-level project builds every game under games/ into <build>/<id>/ and the
# hub menu that starts them into <build>/.
include(ExternalProject)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(TM_GAMES tm1 tm2 tm3 tm4)
set(TM_TITLE_tm1 "Twisted Metal")
set(TM_TITLE_tm2 "Twisted Metal 2")
set(TM_TITLE_tm3 "Twisted Metal III")
set(TM_TITLE_tm4 "Twisted Metal 4")
set(TM_SUB_tm1 "1995  -  SingleTrac")
set(TM_SUB_tm2 "1996  -  SingleTrac")
set(TM_SUB_tm3 "1998  -  989 Studios")
set(TM_SUB_tm4 "1999  -  989 Studios")
# Box art names in the libretro-thumbnails project; the hub downloads them.
set(TM_COVER_tm1 "Twisted Metal (USA)")
set(TM_COVER_tm2 "Twisted Metal 2 (USA)")
set(TM_COVER_tm3 "Twisted Metal III (USA)")
set(TM_COVER_tm4 "Twisted Metal 4 (USA)")
set(TM_EXE_tm1 "TwistedMetal_Recompiled")
set(TM_EXE_tm2 "TwistedMetal2_Recompiled")
set(TM_EXE_tm3 "TwistedMetal3_Recompiled")
set(TM_EXE_tm4 "TwistedMetal4_Recompiled")

set(_tm_cxx_arg)
if(CMAKE_CXX_COMPILER)
    set(_tm_cxx_arg "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
elseif(CMAKE_C_COMPILER MATCHES "clang(\\.exe)?$")
    string(REGEX REPLACE "clang(\\.exe)?$" "clang++\\1" _tm_cxx "${CMAKE_C_COMPILER}")
    set(_tm_cxx_arg "-DCMAKE_CXX_COMPILER=${_tm_cxx}")
endif()

# The recompiled C for a game is generated from your own disc and is not part of
# the repository. A game without it is left out, so you can build only the
# games you own.
set(_tm_games_txt "# Title|Subtitle|executable relative to this file|cover name\n")
set(_tm_built)
foreach(g IN LISTS TM_GAMES)
    file(GLOB _tm_generated "${CMAKE_CURRENT_SOURCE_DIR}/games/${g}/generated/*.c")
    if(NOT _tm_generated)
        message(WARNING "${TM_TITLE_${g}}: no recompiled code in games/${g}/generated. "
                        "Run tools/generate.sh ${g} <your disc image> to include it.")
        continue()
    endif()
    list(APPEND _tm_built ${g})
    ExternalProject_Add(${g}
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/games/${g}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/${g}"
        CMAKE_ARGS
            "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
            ${_tm_cxx_arg}
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target psx-runtime
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
        USES_TERMINAL_CONFIGURE ON
        USES_TERMINAL_BUILD ON)
    string(APPEND _tm_games_txt
        "${TM_TITLE_${g}}|${TM_SUB_${g}}|${g}/${TM_EXE_${g}}.exe|${TM_COVER_${g}}\n")
endforeach()
if(NOT _tm_built)
    message(FATAL_ERROR "No game has recompiled code yet. Generate at least one first:\n"
                        "  tools/generate.sh tm1 \"/path/to/Twisted Metal.chd\"")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/games.txt" "${_tm_games_txt}")

# Default display settings for a fresh build; a player's own file is left alone.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/shared-settings.toml")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/hub/default-settings.toml"
                   "${CMAKE_BINARY_DIR}/shared-settings.toml" COPYONLY)
endif()

if(WIN32)
    # The window/taskbar icon is optional and not committed: put a square PNG at
    # hub/icon/hub.png and run tools/make_icon.ps1 to produce hub/icon/hub.ico.
    set(_tm_hub_sources hub/hub.c)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hub/icon/hub.ico")
        enable_language(RC)
        list(APPEND _tm_hub_sources hub/hub.rc)
    endif()
    add_executable(TwistedMetal1234Recomp WIN32 ${_tm_hub_sources})
    target_compile_definitions(TwistedMetal1234Recomp PRIVATE UNICODE _UNICODE)
    target_link_options(TwistedMetal1234Recomp PRIVATE -municode -static)
    target_link_libraries(TwistedMetal1234Recomp PRIVATE gdiplus msimg32 urlmon)
    # Cover images are not committed: the hub downloads missing ones itself. Any
    # already in hub/covers (tools/fetch_covers.sh) are copied beside it.
    file(GLOB _tm_covers "${CMAKE_CURRENT_SOURCE_DIR}/hub/covers/*.png")
    foreach(_tm_cover IN LISTS _tm_covers)
        get_filename_component(_tm_cover_name "${_tm_cover}" NAME)
        configure_file("${_tm_cover}" "${CMAKE_BINARY_DIR}/covers/${_tm_cover_name}" COPYONLY)
    endforeach()
else()
    message(STATUS "The hub menu is Windows-only; start each game from its own build directory.")
endif()
