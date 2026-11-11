# mk9 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(mk9 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Every player's PC builds its own copy, with or without AVX2: no fused multiply-adds the source
# did not ask for, so every build computes the same floats (netplay compares the games' state)
add_compile_options(-ffp-contract=off)

# the game's release (mk9\VERSION, a player package's project\VERSION): netplay refuses to match two games built
# from different ones. It goes up only when the game changes, and every player builds the game again then; a release
# of the launcher alone (the repository's VERSION) leaves it, and the games already built
foreach(_v "${CMAKE_SOURCE_DIR}/VERSION" "${CMAKE_SOURCE_DIR}/../VERSION")
    if(NOT MK9_VERSION AND EXISTS "${_v}")
        file(READ "${_v}" MK9_VERSION)
        string(STRIP "${MK9_VERSION}" MK9_VERSION)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_v}")  # a new release builds again
    endif()
endforeach()
if(NOT MK9_VERSION)
    set(MK9_VERSION "0.0.0")
endif()
message(STATUS "MK9 game version ${MK9_VERSION}")

include(generated/rexglue.cmake)

# Sources
set(MK9_SOURCES
    src/main.cpp
    src/mk9_app.cpp
    src/mk9/asyncio.cpp
    src/mk9/autopilot.cpp
    src/mk9/controls.cpp
    src/mk9/crash.cpp
    src/mk9/diag.cpp
    src/mk9/engine.cpp
    src/mk9/fibers.cpp
    src/mk9/frame.cpp
    src/mk9/fx.cpp
    src/mk9/game.cpp
    src/mk9/guards.cpp
    src/mk9/gmalloc.cpp
    src/mk9/gpudiag.cpp
    src/mk9/input.cpp
    src/mk9/link.cpp
    src/mk9/names.cpp
    src/mk9/net/netplay.cpp
    src/mk9/net/rollback.cpp
    src/mk9/net/synctest.cpp
    src/mk9/randtrace.cpp
    src/mk9/report.cpp
    src/mk9/simheap.cpp
    src/mk9/snap.cpp
    src/mk9/sound.cpp
    src/mk9/testinput.cpp
)

# The package's ready-made mk9.exe: everything but the game's code, which each player's PC compiles from their own disc
# into mk9game.dll with nothing but the package (mk9/player/README.md). tools\build-player-host.cmd builds this.
option(MK9_PLAYER_HOST "Build mk9.exe without the game's code, for the player package" OFF)
if(MK9_PLAYER_HOST)
    list(APPEND MK9_SOURCES src/mk9/guestlog.cpp)
endif()

if(WIN32)
    add_executable(mk9 WIN32 ${MK9_SOURCES})
else()
    add_executable(mk9 ${MK9_SOURCES})
endif()

if(MK9_PLAYER_HOST)
    target_include_directories(mk9 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${REXGLUE_ENTRYPOINT_INCLUDE_DIR})
    target_link_libraries(mk9 PRIVATE rex::runtime)
    rexglue_configure_target(mk9 GPU_PLUGINS xenos)
    target_compile_definitions(mk9 PRIVATE MK9_PLAYER_HOST=1)
    # what mk9.exe and mk9game.dll take from each other, read from this build's objects just before it links
    set(MK9_LINK_DIR "${CMAKE_CURRENT_BINARY_DIR}/player")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(MK9_LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET mk9 PRE_LINK
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/player/boundary.py"
            --llvm "${MK9_LLVM_BIN}" --config "${CMAKE_CURRENT_SOURCE_DIR}/config/mk9_codegen.toml"
            --out "${MK9_LINK_DIR}" "$<TARGET_OBJECTS:mk9>"
        COMMAND_EXPAND_LISTS VERBATIM)
    target_link_options(mk9 PRIVATE "LINKER:/DEF:${MK9_LINK_DIR}/host_exports.def" "LINKER:${MK9_LINK_DIR}/mk9game.lib")
else()
    rexglue_setup_target(mk9 GPU_PLUGINS xenos)
endif()
target_compile_definitions(mk9 PRIVATE MK9_VERSION="${MK9_VERSION}")

# The SDK built in-tree (REXSDK_DIR) does not pass on ImGui's include directory; its functions
# come from rexruntime.dll (WINDOWS_EXPORT_ALL_SYMBOLS).
if(REXSDK_DIR)
    target_include_directories(mk9 PRIVATE "${REXSDK_DIR}/thirdparty/imgui")
endif()
