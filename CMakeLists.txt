cmake_minimum_required(VERSION 3.25)
project(SacredBild LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "SacredBild must be built for Win32 (x86): Sacred is a 32-bit process.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# RelWithDebInfo is the build that ships: inline like Release (CMake's default for it is /Ob1).
string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

find_package(kcp CONFIG REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h" REQUIRED)
find_library(DETOURS_LIBRARY_RELEASE detours PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(DETOURS_LIBRARY_DEBUG detours PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH REQUIRED)
set(DETOURS_LIBRARY optimized ${DETOURS_LIBRARY_RELEASE} debug ${DETOURS_LIBRARY_DEBUG})

add_library(ddraw SHARED
  src/main.cpp
  src/proxy.cpp
  src/log.cpp
  src/config.cpp
  src/crash_dump.cpp
  src/patch.cpp
  src/profiler.cpp
  src/settings_window.cpp
  src/sig.cpp
  src/system_ddraw.cpp
  src/game/build.cpp
  src/game/focus.cpp
  src/game/frame_hooks.cpp
  src/game/gameserver.cpp
  src/game/granny_async.cpp
  src/game/language.cpp
  src/game/map_cache.cpp
  src/game/ground_quads.cpp
  src/game/skin_check.cpp
  src/game/granny_mesh.cpp
  src/game/gpu_skin.cpp
  src/game/world_passes.cpp
  src/game/movie.cpp
  src/game/d3d_stats.cpp
  src/game/device_proxy.cpp
  src/game/resolution.cpp
  src/game/screenshot.cpp
  src/game/ui_anchor.cpp
  src/game/ui_canvas.cpp
  src/ddraw9/device.cpp
  src/ddraw9/device_skin.cpp
  src/ddraw9/directdraw.cpp
  src/ddraw9/format.cpp
  src/ddraw9/gpu.cpp
  src/ddraw9/surface.cpp
  src/ddraw9/vertex_buffer.cpp
  src/render/atlas.cpp
  src/render/batcher.cpp
  src/ui/form.cpp
  src/ui/form.rc
  src/net/adapters.cpp
  src/net/connection.cpp
  src/net/lan_client.cpp
  src/net/lan_protocol.cpp
  src/net/lan_server.cpp
  src/net/matchmaker.cpp
  src/net/tincat_shim.cpp
  src/net/udp_endpoint.cpp
  src/net/udp_protocol.cpp
  src/net/udp_transport.cpp
  src/exports.def
  src/version.rc
)
target_include_directories(ddraw PRIVATE src ${DETOURS_INCLUDE_DIRS})
target_link_libraries(ddraw PRIVATE ${DETOURS_LIBRARY} kcp::kcp psapi dxguid gdi32 ole32 oleaut32 mfplat mfuuid ws2_32 iphlpapi comctl32 delayimp)
target_compile_definitions(ddraw PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(ddraw PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
# SAFESEH must stay off: Detours trampolines are not registered handlers.
# The map file lets tools/profile_report.py name functions in SacredBild's own samples.
target_link_options(ddraw PRIVATE /SAFESEH:NO /MAP)
# Winsock and the IP helper load on first use (LAN relay), Media Foundation with the first movie, Common Controls
# in the settings window (version 6 there, from its activation context).
target_link_options(ddraw PRIVATE /DELAYLOAD:ws2_32.dll /DELAYLOAD:iphlpapi.dll /DELAYLOAD:mfplat.dll /DELAYLOAD:comctl32.dll)
set_target_properties(ddraw PROPERTIES OUTPUT_NAME "ddraw")

# Optional: copy the DLL into the game folder after build (-DSACRED_GAME_DIR=...).
set(SACRED_GAME_DIR "" CACHE PATH "Sacred install folder to deploy ddraw.dll into after building")
if(SACRED_GAME_DIR)
  add_custom_command(TARGET ddraw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ddraw> "${SACRED_GAME_DIR}/ddraw.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:ddraw> "${SACRED_GAME_DIR}/ddraw.pdb")
endif()

# Test bench for the UDP transport and the matchmaker client outside the game (tools/nettest/nettest.cpp).
option(SACREDBILD_NETTEST "Build tools/nettest" OFF)
if(SACREDBILD_NETTEST)
  add_executable(nettest
    tools/nettest/nettest.cpp
    src/config.cpp
    src/log.cpp
    src/net/adapters.cpp
    src/net/lan_protocol.cpp
    src/net/matchmaker.cpp
    src/net/udp_endpoint.cpp
    src/net/udp_protocol.cpp
    src/net/udp_transport.cpp
  )
  target_include_directories(nettest PRIVATE src)
  target_link_libraries(nettest PRIVATE kcp::kcp ws2_32 iphlpapi winmm)
  target_compile_definitions(nettest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_compile_options(nettest PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
endif()
