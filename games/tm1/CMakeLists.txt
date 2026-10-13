cmake_minimum_required(VERSION 3.20)
# The framework and launcher UI are shared by every game in this repo and live
# at the repository root.
get_filename_component(TM_COLLECTION_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PSXRECOMP_ROOT "${TM_COLLECTION_ROOT}/psxrecomp" CACHE PATH
    "Path to psxrecomp")
set(RECOMP_UI_ROOT "${TM_COLLECTION_ROOT}/recomp-ui" CACHE PATH
    "Path to recomp-ui")
if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/runtime.cmake")
    message(FATAL_ERROR
        "PSXRecomp runtime is missing: ${PSXRECOMP_ROOT}/runtime/runtime.cmake\n"
        "This project expects the psxrecomp framework checkout at ${PSXRECOMP_ROOT}.\n"
        "Run: git submodule update --init --recursive\n"
        "If psxrecomp/ is not a submodule, set -DPSXRECOMP_ROOT=<path-to-psxrecomp>.")
endif()

project(TwistedMetalRecomp LANGUAGES C CXX)

# --- Opt-in product surfaces (OFF by default in the framework) -------------
# Scaffold prompts / flags fill these. Without recomp-ui, PSX_RECOMP_UI is OFF.
# The framework's first-run setup wizard is left off: it locates the project by
# walking up from the executable, and with builds under <repo>/build/<id>/ it
# takes the build folder for the project, finds no generated/ there, and
# offers to regenerate a game that is already built. The game code is
# committed, and the launcher's Disc panel picks the disc.
# recomp-ui submodule present (PSX_RECOMP_UI defaults ON).
# Full netplay UI — enable after adding recomp-ui + testing:
# if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
#     set(PSX_NETPLAY ON CACHE BOOL "…" FORCE)
# endif()
set(PSX_SETUP_WIZARD OFF CACHE BOOL
    "Advertise first-run setup wizard + Generate & rebuild in recomp-ui" FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

# This project's own mod catalog. The FRAMEWORK stages it -- together with its
# builtin psx.* packages -- into <exe-dir>/mods/bundled, and refuses to
# configure if a catalog exists here but is not declared. Never copy it by
# hand: five titles did, and framework 4cc04be3's rename of the staged
# directory broke all five silently (bead beads-eio.3.101).
set(PSX_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")
if(NOT IS_DIRECTORY "${PSX_PRELOADED_MODS}")
    set(PSX_PRELOADED_MODS "NONE")
endif()

psxrecomp_add_game_runtime(psx-runtime
    PRELOADED_MODS_DIR "${PSX_PRELOADED_MODS}"
    # ENABLE_NETPLAY_IF_PRESENT
    # NETPLAY_LOBBY_URL "ws://netplay.retcomm.net:8765"
    WINDOW_TITLE "TwistedMetal Recompiled"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    MAX_PLAYERS 2
    # LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"
    APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"
    # Committed game C (psxrecomp_cli.py generate; git add generated). Release
    # CI configures with -DPSXRECOMP_REQUIRE_GAME_C=ON so a checkout without
    # it fails instead of building a setup host.
    GEN_MARKER "generated/SCUS_943.04_dispatch.c"
    GEN_FULL_GLOB "generated/SCUS_943.04_full_*.c"
    CODEGEN_SETUP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/codegen_setup.c"
)


# Trusted static mod plugins backing the packages under mods/preloaded.
target_sources(psx-runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/mods/src/tm1_framerate.c")

# Stage runtime configs next to the exe for direct build/ launches. This is its
# own always-run target: as a POST_BUILD step it only ran when the executable
# relinked, so a change to game.toml alone never reached the build folder.
set(_PSX_STAGE_CONFIG_COMMANDS)
foreach(_PSX_RUNTIME_CONFIG game.toml game_options.toml)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}")
        list(APPEND _PSX_STAGE_CONFIG_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}"
                "${CMAKE_CURRENT_BINARY_DIR}/${_PSX_RUNTIME_CONFIG}")
    endif()
endforeach()
add_custom_target(stage-runtime-config ${_PSX_STAGE_CONFIG_COMMANDS}
    COMMENT "Staging game.toml" VERBATIM)
add_dependencies(psx-runtime stage-runtime-config)
