# vcpkg's community x86-windows-static triplet (32-bit, static libraries and CRT), with SDL3 built for what SacredBild
# uses of it: joysticks and gamepads (input/gamepad.cpp). Picked up through VCPKG_OVERLAY_TRIPLETS (CMakePresets.json).
set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "sdl3")
    # No audio, video (so no GPU, render, camera), haptics, power, sensors, dialogs or tray. SDL only looks for
    # dinput.h when audio or video is on; HAVE_DINPUT_H keeps DirectInput for pads that are neither XInput nor HIDAPI
    # ones. XInput, raw input, Windows.Gaming.Input and HIDAPI need nothing else.
    set(VCPKG_CMAKE_CONFIGURE_OPTIONS
        -DSDL_AUDIO=OFF
        -DSDL_VIDEO=OFF
        -DSDL_GPU=OFF
        -DSDL_RENDER=OFF
        -DSDL_CAMERA=OFF
        -DSDL_HAPTIC=OFF
        -DSDL_POWER=OFF
        -DSDL_SENSOR=OFF
        -DSDL_DIALOG=OFF
        -DSDL_TRAY=OFF
        -DHAVE_DINPUT_H=1
    )
    # SDL's dynamic API (a table of every SDL function, so an SDL3.dll named by an environment variable can stand in)
    # keeps the whole library in the link. SDL_dynapi.h turns it off only for a few environments, an editor's code
    # analysis among them; __RESHARPER__ is checked there and nowhere else (SDL, Windows SDK, MSVC headers).
    set(VCPKG_C_FLAGS "/D__RESHARPER__")
    set(VCPKG_CXX_FLAGS "/D__RESHARPER__")
endif()
