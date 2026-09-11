# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP): Stub - effects bypass
# Encode Processor (EP): Stub - direct mixbin passthrough

set(XBOX_APU_SOURCES
    apu_core.c
    apu_vp.c
    apu_dsp.c
    apu_mmio_hook.c
)
if(APPLE)
    # CoreAudio, which is in the system on macOS and iOS alike. The XAudio2
    # file compiles to nothing here and waveOut does not exist at all, so the
    # AudioQueue backend answers the same five entry points instead.
    list(APPEND XBOX_APU_SOURCES apu_coreaudio.c)
else()
    list(APPEND XBOX_APU_SOURCES apu_xaudio2.c)
endif()

add_library(xbox_apu STATIC ${XBOX_APU_SOURCES})

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

if(WIN32)
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm)
elseif(APPLE)
    target_link_libraries(xbox_apu PRIVATE platform
        "-framework AudioToolbox" "-framework CoreFoundation" m)
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL2 audio + libm
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL2::SDL2 m)
endif()
