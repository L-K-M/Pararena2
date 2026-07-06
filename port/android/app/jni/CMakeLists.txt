cmake_minimum_required(VERSION 3.16)
project(pararena2_android C)

# SDL3 source is staged here by fetch-deps.sh (its CMake builds libSDL3.so for
# Android and defines the SDL3::SDL3 target the game links against).
add_subdirectory(SDL)

# Build the game from the shared port CMakeLists. On Android it produces a
# shared library whose output name is "main" (libmain.so) — the second library
# SDLActivity loads after libSDL3.so. Because SDL3::SDL3 already exists, the port
# CMakeLists skips its own find_package/FetchContent of SDL.
#   this dir = port/android/app/jni   ->   ../../.. = port
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../.. pararena2)
