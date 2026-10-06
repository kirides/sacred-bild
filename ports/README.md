# vcpkg overlay ports

`sdl3`: vcpkg's port (as of the vcpkg commit the build uses) with `fix-rawinput-xboxone-hid.patch`. SDL's raw input
joystick driver assumed the Xbox 360 HID layout for XInput-capable pads (both triggers on Z) and sorted the HID axes by
usage alone. An Xbox One / Series pad behind a driver that hides it from XInput (e.g. Steam's Xbox extended feature
driver with Steam closed) presents the Xbox One layout instead: X, Y, Rx, Ry, a 10-bit trigger each on Z and Rz, and
a battery strength on the device controls page, which sorted first and shifted every axis by one. The patch sorts by
usage page first, ignores non-axis pages for XInput-capable pads, and reads split triggers from Z and Rz scaled by their
logical range.
