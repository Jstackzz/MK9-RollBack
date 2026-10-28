cmake_minimum_required(VERSION 3.21)
# the release (the repository's VERSION file); the game has its own, mk9\VERSION, raised only when the game changes
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" MK9_RELEASE)
string(STRIP "${MK9_RELEASE}" MK9_RELEASE)
project(mk9_launcher VERSION ${MK9_RELEASE} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

# SDL3 for controllers (GamepadNav), static so it links into the exe: the game's own copy of the sources (the
# ReXGlue SDK's thirdparty/sdl3), so the launcher reads the same pads the same way and nothing is downloaded.
set(MK9_SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/rexglue-sdk/thirdparty/sdl3" CACHE PATH
    "SDL3 sources (default: the ReXGlue SDK's copy)")
if(NOT EXISTS "${MK9_SDL3_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SDL3 sources not found in ${MK9_SDL3_DIR}: set -DMK9_SDL3_DIR=<SDL3 source folder> "
                        "(see docs/LAUNCHER.md)")
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory("${MK9_SDL3_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/sdl3" EXCLUDE_FROM_ALL)

qt_add_executable(MK9Launcher WIN32
    src/main.cpp
    src/AppPaths.cpp src/AppPaths.h
    src/WinUtil.cpp src/WinUtil.h
    src/GameData.cpp src/GameData.h
    src/Style.cpp src/Style.h
    src/ServerClient.cpp src/ServerClient.h
    src/UdpRendezvous.cpp src/UdpRendezvous.h
    src/GameProcess.cpp src/GameProcess.h
    src/MatchController.cpp src/MatchController.h
    src/FeedLink.cpp src/FeedLink.h
    src/GamepadNav.cpp src/GamepadNav.h
    src/Widgets.cpp src/Widgets.h
    src/Dialogs.cpp src/Dialogs.h
    src/PlayPages.cpp src/PlayPages.h
    src/LobbyPage.cpp src/LobbyPage.h
    src/ToolPages.cpp src/ToolPages.h
    src/Controls.cpp src/Controls.h
    src/ControlsPage.cpp src/ControlsPage.h
    src/Xiso.cpp src/Xiso.h
    src/Upnp.cpp src/Upnp.h
    src/Updater.cpp src/Updater.h
    src/GitHubUpdates.cpp src/GitHubUpdates.h
    src/MainWindow.cpp src/MainWindow.h
)

qt_add_resources(MK9Launcher "gamedata"
    PREFIX "/data"
    BASE data
    FILES data/mk9.json
)

target_compile_definitions(MK9Launcher PRIVATE
    MK9_LAUNCHER_VERSION="${PROJECT_VERSION}"
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(MK9Launcher PRIVATE Qt6::Widgets Qt6::Network SDL3::SDL3-static dwmapi advapi32)
set_target_properties(MK9Launcher PROPERTIES OUTPUT_NAME "MK9Launcher")

if(MSVC)
    target_compile_options(MK9Launcher PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
endif()

# Copy the Qt runtime next to the exe:  cmake --build <dir> --target deploy
find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
if(WINDEPLOYQT)
    add_custom_target(deploy
        COMMAND "${WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                --no-compiler-runtime "$<TARGET_FILE:MK9Launcher>"
        DEPENDS MK9Launcher
        COMMENT "windeployqt MK9Launcher"
        VERBATIM)
endif()
