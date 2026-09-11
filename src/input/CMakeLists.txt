# Xbox Input → XInput Compatibility Layer

set(XBOX_INPUT_SOURCES xinput_device.c)
if(APPLE)
    # GameController.framework is in the system on both macOS and iOS and
    # speaks to a Bluetooth DualSense with nothing installed, which SDL2
    # cannot do here -- Homebrew only ships an x86_64 build of it.
    list(APPEND XBOX_INPUT_SOURCES xinput_gc.m)
endif()

add_library(xbox_input STATIC ${XBOX_INPUT_SOURCES})

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput)
elseif(APPLE)
    target_link_libraries(xbox_input PUBLIC platform
        "-framework GameController" "-framework Foundation")
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
