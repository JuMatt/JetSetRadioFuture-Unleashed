cmake_minimum_required(VERSION 3.20)

# JSRF Unleashed -- the game, built from this repository.
#
# The generated code is not in the repository (it is derived from the game's
# executable): produce it from your own default.xbe with the pipeline in
# tools/recomp, into port/recomp/gen (the folder .gitignore keeps out of git),
# or point JSRF_GEN_DIR at a folder named recomp/gen elsewhere. Then:
#
#   cmake -S port -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# On Linux add -DNV2A_GL_CONTEXT=sdl (a window, sound and pads through SDL2);
# the Mac uses its own frameworks by default.
#
# The version is the release tag this tree is at (git describe: "0.1.2", or
# "0.1.2-3-gabc1234-dirty" between releases); a tree without its git history
# (an export) passes it with -DJSRF_VERSION=. The engine prints it on its
# first line and answers --version; the launchers take it from there.

# The Mac build is Apple Silicon only. Said before project(), so that a cmake
# running under Rosetta (an Intel Homebrew's) does not quietly build x86_64.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Mac architecture" FORCE)
endif()

project(jsrf_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    RECOMP_ICALL_FEEDBACK
    RECOMP_ABI_CHECK
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

get_filename_component(JSRF_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set(JSRF_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recomp/gen" CACHE PATH
    "The generated code: a folder named recomp/gen")
if(NOT EXISTS "${JSRF_GEN_DIR}/recomp_types.h")
    message(FATAL_ERROR "No generated code in ${JSRF_GEN_DIR}. Generate it from your "
                        "own default.xbe with tools/recomp (see docs/GETTING_STARTED.md), "
                        "or set JSRF_GEN_DIR.")
endif()
# recomp_manual.c includes "recomp/gen/recomp_types.h": the folder two
# levels above the generated code goes on the include path.
get_filename_component(JSRF_GEN_ROOT "${JSRF_GEN_DIR}/../.." ABSOLUTE)

set(JSRF_VERSION "" CACHE STRING "Version (default: git describe)")
if(NOT JSRF_VERSION)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --dirty
                        WORKING_DIRECTORY "${JSRF_REPO}"
                        OUTPUT_VARIABLE _jsrf_describe OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE _jsrf_describe_rc)
        if(_jsrf_describe_rc EQUAL 0)
            string(REGEX REPLACE "^v" "" JSRF_VERSION "${_jsrf_describe}")
        endif()
    endif()
endif()
if(NOT JSRF_VERSION)
    set(JSRF_VERSION "dev")
endif()
message(STATUS "JSRF Unleashed version: ${JSRF_VERSION}")

# The runtime: everything in this repository outside port/ and launcher/.
add_subdirectory("${JSRF_REPO}" "${CMAKE_BINARY_DIR}/xboxrecomp")

file(GLOB JSRF_GEN_SOURCES CONFIGURE_DEPENDS "${JSRF_GEN_DIR}/*.c")

add_executable(jsrf_recomp
    main.c
    recomp_manual.c
    ${JSRF_GEN_SOURCES}
)
target_compile_definitions(jsrf_recomp PRIVATE JSRF_VERSION="${JSRF_VERSION}")
target_link_libraries(jsrf_recomp PRIVATE xboxrecomp)
target_include_directories(jsrf_recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${JSRF_GEN_ROOT}"
    "${JSRF_GEN_DIR}"
    "${JSRF_REPO}/include"
)
if(WIN32)
    target_link_libraries(jsrf_recomp PRIVATE d3d11 dxgi dxguid xinput winmm dbghelp)
else()
    target_link_libraries(jsrf_recomp PRIVATE pthread m dl)
    target_link_options(jsrf_recomp PRIVATE -rdynamic)
endif()
if(MSVC)
    target_link_options(jsrf_recomp PRIVATE /LARGEADDRESSAWARE)
    target_compile_options(jsrf_recomp PRIVATE /bigobj)
endif()
