cmake_minimum_required(VERSION 3.16)
project(pararena2 C)

set(CMAKE_C_STANDARD 99)

# ---------------------------------------------------------------- SDL3
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not found - fetching and building it")
    include(FetchContent)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://www.libsdl.org/release/SDL3-3.4.12.tar.gz)
    FetchContent_MakeAvailable(SDL3)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(GAME_SRC_DIR "${CMAKE_CURRENT_BINARY_DIR}/game")

# ------------------------------------------------- original sources, verbatim
# (converted at build time: CR line endings -> LF, "\p" Pascal literals)
set(GAME_SOURCES
    Main.c
    PlayCore.c
    PlayUtils.c
    Render.c
    RenderQD.c
    Replay.c
    Human.c
    Ball.c
    CommonPerson.c
    Computer.c
    Dynamics.c
    InitGameStructs.c
    MainWindow.c
    SoundUtils.c
)

set(CONVERTED_SOURCES "")
foreach(f ${GAME_SOURCES})
    list(APPEND CONVERTED_SOURCES "${GAME_SRC_DIR}/${f}")
endforeach()

file(GLOB ORIGINAL_HEADERS "${REPO_ROOT}/Headers/*.h")
set(ORIGINAL_SOURCE_PATHS "")
foreach(f ${GAME_SOURCES})
    list(APPEND ORIGINAL_SOURCE_PATHS "${REPO_ROOT}/Sources/${f}")
endforeach()

add_custom_command(
    OUTPUT ${CONVERTED_SOURCES} "${GAME_SRC_DIR}/Globals.h"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/convert_sources.py"
            "${REPO_ROOT}" "${GAME_SRC_DIR}" ${GAME_SOURCES}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/convert_sources.py"
            ${ORIGINAL_SOURCE_PATHS} ${ORIGINAL_HEADERS}
    COMMENT "Converting original THINK C sources"
    VERBATIM)

# ------------------------------------------------- asset pack (if missing)
set(ASSET_PACK "${CMAKE_CURRENT_SOURCE_DIR}/assets/pararena2.dat")
if(NOT EXISTS "${ASSET_PACK}")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_assets.py"
        RESULT_VARIABLE ASSET_RC)
    if(NOT ASSET_RC EQUAL 0)
        message(FATAL_ERROR "asset extraction failed")
    endif()
endif()

# ------------------------------------------------- executable
add_executable(pararena2
    ${CONVERTED_SOURCES}
    shim/shim_qd.c
    shim/shim_text.c
    shim/shim_res.c
    shim/shim_os.c
    shim/shim_input.c
    shim/shim_sound.c
    src/port_main.c
    src/port_init.c
    src/port_shell.c
    src/port_four.c
    src/port_video.c
    src/port_univ.c
    src/port_prefs.c
    src/port_stubs.c
)

target_include_directories(pararena2 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/shim"
    "${GAME_SRC_DIR}")

target_link_libraries(pararena2 PRIVATE SDL3::SDL3)

# every original file gets the shim force-included and main renamed
if(MSVC)
    set(GAME_FLAGS "/FIMacShim.h;/Dmain=PararenaMain;/wd4068")
else()
    # newer clang (Xcode 16+) makes implicit-int a hard error; the 1992
    # sources declare main() K&R-style, so demote it back to a warning
    set(GAME_FLAGS "-include;MacShim.h;-Dmain=PararenaMain;-Wno-multichar;-Wno-parentheses;-Wno-comment;-Wno-dangling-else;-Wno-error=implicit-int;-Wno-error=implicit-function-declaration")
    target_compile_options(pararena2 PRIVATE -fsigned-char)
endif()
set_source_files_properties(${CONVERTED_SOURCES} PROPERTIES
    COMPILE_OPTIONS "${GAME_FLAGS}")

# copy the asset pack next to the binary
add_custom_command(TARGET pararena2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ASSET_PACK}" "$<TARGET_FILE_DIR:pararena2>/pararena2.dat")
