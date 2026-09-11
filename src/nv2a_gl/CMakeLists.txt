# NV2A -> OpenGL backend.
#
# Separate from xbox_nv2a (register-level emulation) and from nv2a_pb_exec
# (the software rasteriser): this one turns the same pushbuffer into real
# draw calls. Built only where EGL and GL are available; on a host without
# them the runtime keeps the software path and nothing else changes.
set(NV2A_GL_SOURCES
    nv2a_vsh.c
    nv2a_vsh_interp.c
    nv2a_psh.c
    nv2a_gl.c
)
if(APPLE)
    # An AppKit window, so the frame can be watched rather than only dumped.
    # Cocoa is in the system; the only SDL2 on an Intel-Homebrew Mac is an
    # x86_64 build that an arm64 binary cannot link against.
    list(APPEND NV2A_GL_SOURCES nv2a_window_mac.m)
    # The Metal backend. Built here rather than in a library of its own
    # because it is the other implementation of nv2a_backend.h and the
    # selection between the two is a getenv in nv2a_gl.c -- two libraries
    # would mean a link-time choice for something that wants to be a run-time
    # one. NV2A_HAVE_METAL says the symbol exists; without it nv2a_gl.c
    # supplies a nv2a_backend_metal() that returns NULL, which is what the
    # header asks a backend that cannot be built to do.
    list(APPEND NV2A_GL_SOURCES ../nv2a_metal/nv2a_metal.m)
endif()

add_library(xbox_nv2a_gl STATIC ${NV2A_GL_SOURCES})

target_include_directories(xbox_nv2a_gl PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../d3d
)

if(APPLE)
    target_compile_definitions(xbox_nv2a_gl PRIVATE NV2A_HAVE_METAL)
    target_link_libraries(xbox_nv2a_gl PUBLIC
        "-framework Metal" "-framework Foundation" "-framework QuartzCore")
endif()

#
# How the context is obtained.
#
# EGL with a surfaceless display is the headless default: no window server
# needed, which is what a build box wants. It does not exist on macOS, so the
# Apple build goes through SDL2, which also works here -- selecting SDL on
# Linux is how that path gets tested before it reaches a Mac.
#
#   cmake -DNV2A_GL_CONTEXT=sdl ..
#
# cgl is the Apple default: it is in the system OpenGL framework, so it needs
# nothing installed -- which matters on a Mac whose Homebrew is the Intel one,
# where every package is an x86_64 library an arm64 binary cannot link.
if(APPLE AND NOT DEFINED NV2A_GL_CONTEXT)
    set(NV2A_GL_CONTEXT "cgl")
elseif(NOT DEFINED NV2A_GL_CONTEXT)
    set(NV2A_GL_CONTEXT "egl")
endif()

find_package(OpenGL REQUIRED)

if(NV2A_GL_CONTEXT STREQUAL "cgl")
    target_compile_definitions(xbox_nv2a_gl PUBLIC NV2A_GL_USE_CGL=1)
    target_link_libraries(xbox_nv2a_gl PUBLIC
        "-framework OpenGL" "-framework Cocoa" m)
elseif(NV2A_GL_CONTEXT STREQUAL "sdl")
    find_package(SDL2 REQUIRED)
    target_compile_definitions(xbox_nv2a_gl PUBLIC NV2A_GL_USE_SDL=1)
    target_include_directories(xbox_nv2a_gl PUBLIC ${SDL2_INCLUDE_DIRS})
    if(TARGET SDL2::SDL2)
        target_link_libraries(xbox_nv2a_gl PUBLIC OpenGL::GL SDL2::SDL2 m)
    else()
        target_link_libraries(xbox_nv2a_gl PUBLIC OpenGL::GL ${SDL2_LIBRARIES} m)
    endif()
else()
    target_link_libraries(xbox_nv2a_gl PUBLIC OpenGL::GL EGL m)
endif()
message(STATUS "nv2a_gl context: ${NV2A_GL_CONTEXT}")

if(MSVC)
    target_compile_options(xbox_nv2a_gl PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_nv2a_gl PRIVATE -Wall -Wextra -O2)
endif()
