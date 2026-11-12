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

# Derive the small input wrapper locally; never check generated game code in.
# This also handles an external JSRF_GEN_DIR and fresh _gen wrapper names.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(JSRF_CAMERA_NATIVE_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/jsrf_camera_native_build.inc")
add_custom_command(
    OUTPUT "${JSRF_CAMERA_NATIVE_INCLUDE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_camera_native.py"
            --gen-dir "${JSRF_GEN_DIR}" --output "${JSRF_CAMERA_NATIVE_INCLUDE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prepare_camera_native.py"
            "${JSRF_GEN_DIR}/recomp_0002.c"
    COMMENT "Verifying native input preimage and deriving the private camera wrapper"
    VERBATIM
)
add_custom_target(jsrf_camera_native DEPENDS "${JSRF_CAMERA_NATIVE_INCLUDE}")

option(JSRF_CAMERA_TESTS "Build persistent-camera offline regression tests" OFF)
if(JSRF_CAMERA_TESTS)
    enable_testing()
    add_executable(camera_intent_test tests/camera_intent_test.c)
    if(NOT WIN32)
        target_link_libraries(camera_intent_test PRIVATE m)
    endif()
    add_test(NAME camera_intent COMMAND camera_intent_test)
    if(WIN32)
        add_executable(camera_bridge_test tests/camera_bridge_test.c)
        target_include_directories(camera_bridge_test PRIVATE "${JSRF_REPO}/src")
        add_executable(camera_trace_test tests/camera_trace_test.c)
        add_test(NAME camera_bridge COMMAND camera_bridge_test)
        add_test(NAME camera_trace COMMAND camera_trace_test "${CMAKE_CURRENT_BINARY_DIR}/camera-trace-fixture.csv")
        add_test(NAME camera_trace_arm COMMAND camera_trace_test "${CMAKE_CURRENT_BINARY_DIR}/camera-trace-arm.csv" "${CMAKE_CURRENT_BINARY_DIR}/camera-trace-arm.marker")
        add_test(NAME camera_trace_rail COMMAND camera_trace_test "${CMAKE_CURRENT_BINARY_DIR}/camera-trace-rail.csv" "${CMAKE_CURRENT_BINARY_DIR}/camera-trace-rail.marker" rail)
    endif()
    # Fixtures use assert for both verification and checked setup calls.
    foreach(_camera_test camera_intent_test camera_bridge_test camera_trace_test)
        if(TARGET ${_camera_test})
            if(MSVC)
                target_compile_options(${_camera_test} PRIVATE /UNDEBUG)
            else()
                target_compile_options(${_camera_test} PRIVATE -UNDEBUG)
            endif()
        endif()
    endforeach()
endif()

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

# Windows (MinGW): the runtime's OpenGL / SDL2 host layers, as on Linux and the
# Mac, rather than its Direct3D 11 ones -- the title draws through its own D3D
# and the pushbuffer, which is what nv2a_gl renders everywhere else.
if(WIN32)
    set(XBOXRECOMP_WIN_GL ON)
    add_compile_definitions(XBOXRECOMP_WIN_GL=1)
endif()

# Android: plain char is signed on x86 and on Apple's arm64, everywhere this
# has run so far; the Arm ABI that Android follows makes it unsigned. Signed,
# as it was tested.
if(ANDROID)
    add_compile_options(-fsigned-char)
endif()

# The runtime: everything in this repository outside port/ and launcher/.
add_subdirectory("${JSRF_REPO}" "${CMAKE_BINARY_DIR}/xboxrecomp")

file(GLOB JSRF_GEN_SOURCES CONFIGURE_DEPENDS "${JSRF_GEN_DIR}/*.c")

# recomp_manual.c wraps some generated functions: it declares the generated
# body as sub_X_gen ("extern void sub_X_gen(void);") and defines sub_X itself.
# tools/recomp names such a body sub_X_gen when it generates the code; code
# generated before a wrap was added still calls it sub_X. A generated file
# like that is compiled from a copy in the build folder with the body renamed,
# so older generated code -- the release builds' included -- needs no editing
# by hand. Files that need nothing are compiled where they are.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/recomp_manual.c" _jsrf_wrap_lines
     REGEX "^extern void sub_[0-9A-Fa-f]+_gen\\(void\\);")
set(_jsrf_wraps "")
foreach(_l IN LISTS _jsrf_wrap_lines)
    string(REGEX REPLACE "^extern void (sub_[0-9A-Fa-f]+)_gen\\(void\\);.*$" "\\1" _n "${_l}")
    list(APPEND _jsrf_wraps "${_n}")
endforeach()
set(_jsrf_gen_sources "")
foreach(_src IN LISTS JSRF_GEN_SOURCES)
    file(READ "${_src}" _txt)
    set(_renamed "")
    foreach(_n IN LISTS _jsrf_wraps)
        string(FIND "${_txt}" "\nvoid ${_n}(void)\n" _at)
        if(NOT _at EQUAL -1)
            string(REPLACE "\nvoid ${_n}(void)\n" "\nvoid ${_n}_gen(void)\n" _txt "${_txt}")
            list(APPEND _renamed "${_n}")
        endif()
    endforeach()
    if(_renamed)
        get_filename_component(_name "${_src}" NAME)
        set(_copy "${CMAKE_CURRENT_BINARY_DIR}/gen_wrapped/${_name}")
        file(WRITE "${_copy}.new" "${_txt}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_copy}.new" "${_copy}")
        file(REMOVE "${_copy}.new")
        list(APPEND _jsrf_gen_sources "${_copy}")
        message(STATUS "Generated code: ${_name} compiled with ${_renamed} renamed _gen (wrapped in recomp_manual.c)")
    else()
        list(APPEND _jsrf_gen_sources "${_src}")
    endif()
    unset(_txt)
endforeach()
set(JSRF_GEN_SOURCES ${_jsrf_gen_sources})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/recomp_manual.c")

# The release builds' generated code carries two tables of functions an
# earlier discovery pass missed (recomp_seeded.c, recomp_holes.c; see
# recomp_manual.c). Code generated with today's tools/recomp has neither:
# empty ones stand in, so both kinds of generated code build.
set(JSRF_TABLES_NONE "")
if(NOT EXISTS "${JSRF_GEN_DIR}/recomp_seeded.c")
    list(APPEND JSRF_TABLES_NONE JSRF_NO_SEEDED)
endif()
if(NOT EXISTS "${JSRF_GEN_DIR}/recomp_holes.c")
    list(APPEND JSRF_TABLES_NONE JSRF_NO_HOLES)
endif()
if(JSRF_TABLES_NONE)
    list(APPEND JSRF_GEN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/recomp_tables_none.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/recomp_tables_none.c"
        PROPERTIES COMPILE_DEFINITIONS "${JSRF_TABLES_NONE}")
endif()

if(ANDROID)
    # Android: the engine is libmain.so, which SDL's Java side loads and whose
    # SDL_main it calls (main.c; android/jsrf_android.c sets the scene). The
    # app around it -- launcher/android -- is built by launcher/android/build_apk.sh.
    add_library(jsrf_recomp SHARED
        main.c
        recomp_manual.c
        xbe_check.c
        android/jsrf_android.c
        ios/xiso.c
        ${JSRF_GEN_SOURCES}
    )
    set_target_properties(jsrf_recomp PROPERTIES OUTPUT_NAME main)
else()
    add_executable(jsrf_recomp
        main.c
        recomp_manual.c
        xbe_check.c
        ios/xiso.c
        ${JSRF_GEN_SOURCES}
    )
endif()
# The shader seed (src/nv2a_gl/nv2a_gl.c, seed_import): the states of the
# shaders the game draws with, collected by playing it, carried in the engine
# as bytes made from port/shader_seed.dat at configure time -- none without it.
# Only rewritten when it changes, so a reconfigure does not rebuild it.
set(JSRF_SEED_DAT "${CMAKE_CURRENT_SOURCE_DIR}/shader_seed.dat")
set(JSRF_SEED_C "${CMAKE_CURRENT_BINARY_DIR}/shader_seed.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${JSRF_SEED_DAT}")
if(EXISTS "${JSRF_SEED_DAT}")
    file(READ "${JSRF_SEED_DAT}" _jsrf_seed_hex HEX)
    string(LENGTH "${_jsrf_seed_hex}" _jsrf_seed_n)
    math(EXPR _jsrf_seed_len "${_jsrf_seed_n} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _jsrf_seed_bytes "${_jsrf_seed_hex}")
    # 24 bytes a line (CMake's expressions have no {n}: the pattern spelt out)
    string(REPEAT "0x[0-9a-f][0-9a-f]," 24 _jsrf_seed_line)
    string(REGEX REPLACE "(${_jsrf_seed_line})" "\\1\n" _jsrf_seed_bytes "${_jsrf_seed_bytes}")
    message(STATUS "Shader seed: ${_jsrf_seed_len} bytes")
else()
    set(_jsrf_seed_len 0)
    set(_jsrf_seed_bytes "0")
    message(STATUS "Shader seed: none (no port/shader_seed.dat)")
endif()
file(WRITE "${JSRF_SEED_C}.new"
    "/* Made by port/CMakeLists.txt from port/shader_seed.dat. */\n"
    "#include <stddef.h>\n"
    "const unsigned char jsrf_shader_seed[] = {\n${_jsrf_seed_bytes}\n};\n"
    "const size_t jsrf_shader_seed_len = ${_jsrf_seed_len};\n")
configure_file("${JSRF_SEED_C}.new" "${JSRF_SEED_C}" COPYONLY)
target_sources(jsrf_recomp PRIVATE "${JSRF_SEED_C}")

if(IOS)
    # The iPhone / iPad app around the engine (ios/jsrf_ios.m: the window,
    # the controls, the game in Documents; the disc-image unpacker, ios/xiso.c,
    # is in every build -- the Mac and Linux apps use it through the engine). The
    # bundle -- Info.plist, icons, signature -- is put together by
    # launcher/ios/package_ios.sh from this executable.
    target_sources(jsrf_recomp PRIVATE ios/jsrf_ios.m)
    set_source_files_properties(ios/jsrf_ios.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set_target_properties(jsrf_recomp PROPERTIES MACOSX_BUNDLE OFF)
    target_link_libraries(jsrf_recomp PRIVATE
        "-framework UIKit" "-framework GameController" "-framework AVFoundation"
        "-framework Metal" "-framework QuartzCore" "-framework CoreGraphics"
        "-framework Foundation")
endif()
target_compile_definitions(jsrf_recomp PRIVATE JSRF_VERSION="${JSRF_VERSION}")
target_link_libraries(jsrf_recomp PRIVATE xboxrecomp)
target_include_directories(jsrf_recomp PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${JSRF_GEN_ROOT}"
    "${JSRF_GEN_DIR}"
    "${JSRF_REPO}/include"
)
add_dependencies(jsrf_recomp jsrf_camera_native)
if(WIN32 AND XBOXRECOMP_WIN_GL)
    # The icon, a manifest (UTF-8 file names, a sharp window on a high-DPI
    # screen) and version information, shared with the launcher
    # (launcher/windows).
    enable_language(RC)
    string(REGEX MATCH "^([0-9]+)[.]([0-9]+)[.]([0-9]+)" _jsrf_v "${JSRF_VERSION}")
    if(_jsrf_v)
        set(_jsrf_vnum "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
    else()
        set(_jsrf_vnum "0,0,0,0")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/jsrf_version.h"
         "#define JSRF_VER_NUM ${_jsrf_vnum}\n#define JSRF_VER_STR \"${JSRF_VERSION}\"\n")
    target_sources(jsrf_recomp PRIVATE "${JSRF_REPO}/launcher/windows/resources.rc")
    target_include_directories(jsrf_recomp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    set_source_files_properties("${JSRF_REPO}/launcher/windows/resources.rc" PROPERTIES
        COMPILE_DEFINITIONS "JSRF_ENGINE")
    # An 8 MB stack for the main thread and for the threads the title creates
    # (CreateThread with size 0 takes the image's), as a Linux main thread has:
    # recompiled functions nest as deeply as the guest's calls, and Windows'
    # default is 1 MB.
    find_package(Threads REQUIRED)
    target_link_libraries(jsrf_recomp PRIVATE Threads::Threads winmm dbghelp)
    target_link_options(jsrf_recomp PRIVATE -Wl,--stack,0x800000)
    # The linker map names the function at any jsrf_recomp.exe+offset a crash
    # report prints (kept with a release build, not shipped).
    target_link_options(jsrf_recomp PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/jsrf_recomp.map")
elseif(WIN32)
    target_link_libraries(jsrf_recomp PRIVATE d3d11 dxgi dxguid xinput winmm dbghelp)
elseif(ANDROID)
    # pthreads are in bionic's libc; log and android for the glue.
    target_include_directories(jsrf_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ios")
    target_link_libraries(jsrf_recomp PRIVATE m dl log android)
    # -Bsymbolic: the game's functions call one another directly rather than
    # through the PLT, as in an executable (nothing ever interposes on them);
    # they stay in the dynamic symbol table, which is what names them in a
    # crash report.
    target_link_options(jsrf_recomp PRIVATE -Wl,--no-undefined -Wl,-Bsymbolic)
else()
    target_link_libraries(jsrf_recomp PRIVATE pthread m dl)
    target_link_options(jsrf_recomp PRIVATE -rdynamic)
endif()
if(MSVC)
    target_link_options(jsrf_recomp PRIVATE /LARGEADDRESSAWARE)
    target_compile_options(jsrf_recomp PRIVATE /bigobj)
endif()
