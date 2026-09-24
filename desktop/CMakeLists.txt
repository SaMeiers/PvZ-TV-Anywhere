# Desktop player: runs the game's original ARM libraries on a PC.
#
#   cmake -S desktop -B build/desktop
#   cmake --build build/desktop --config Release
#
# Pass -DPVZTV_DEPS_DIR=<dir> to build against dependency copies you already
# have instead of the ones in third_party/ (see third_party/CMakeLists.txt).

cmake_minimum_required(VERSION 3.20)
project(pvztv_desktop LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(WIN32)
    # Keep <windows.h> from defining min/max and the rest of the world.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

get_filename_component(PVZTV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_subdirectory("${PVZTV_ROOT}/third_party" "${CMAKE_BINARY_DIR}/third_party")
add_subdirectory("${PVZTV_ROOT}/runner" "${CMAKE_BINARY_DIR}/runner")

add_executable(pvztv_player main.cpp)
target_link_libraries(pvztv_player PRIVATE pvztv_runner)
if(PVZTV_DIAGNOSTICS)
    # So both builds can live in the same game folder.
    set_target_properties(pvztv_player PROPERTIES OUTPUT_NAME pvztv_player-diag)
endif()
target_include_directories(pvztv_player PRIVATE "${PVZTV_ZLIB_INCLUDE_DIR}")
