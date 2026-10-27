# mk9 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(mk9 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(MK9_SOURCES
    src/main.cpp
    src/mk9_app.cpp
    src/mk9/controls.cpp
    src/mk9/frame.cpp
    src/mk9/input.cpp
    src/mk9/snap.cpp
    src/mk9/testinput.cpp
)

if(WIN32)
    add_executable(mk9 WIN32 ${MK9_SOURCES})
else()
    add_executable(mk9 ${MK9_SOURCES})
endif()

rexglue_setup_target(mk9 GPU_PLUGINS xenos)

# The SDK built in-tree (REXSDK_DIR) does not pass on ImGui's include directory; its functions
# come from rexruntime.dll (WINDOWS_EXPORT_ALL_SYMBOLS).
if(REXSDK_DIR)
    target_include_directories(mk9 PRIVATE "${REXSDK_DIR}/thirdparty/imgui")
endif()
