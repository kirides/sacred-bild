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
# ... and link like Release: CMake's default incremental linking pads every function and keeps unreferenced ones.
string(REPLACE "/INCREMENTAL" "/INCREMENTAL:NO /OPT:REF /OPT:ICF" CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO}")

find_package(imgui CONFIG REQUIRED)
find_package(kcp CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
# RmlUi (Sacred Gear's HTML/CSS-style mod UI, gear/rml.cpp) and stb_image (the images its documents name).
find_package(RmlUi CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h" REQUIRED)
find_library(DETOURS_LIBRARY_RELEASE detours PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(DETOURS_LIBRARY_DEBUG detours PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH REQUIRED)
set(DETOURS_LIBRARY optimized ${DETOURS_LIBRARY_RELEASE} debug ${DETOURS_LIBRARY_DEBUG})
# LuaJIT (Sacred Gear): vcpkg installs its headers under include/luajit and the library as lua51.lib.
find_path(LUAJIT_INCLUDE_DIRS "luajit/lua.hpp" REQUIRED)
find_library(LUAJIT_LIBRARY_RELEASE NAMES lua51 luajit-5.1 luajit PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(LUAJIT_LIBRARY_DEBUG NAMES lua51 luajit-5.1 luajit PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH REQUIRED)
set(LUAJIT_LIBRARY optimized ${LUAJIT_LIBRARY_RELEASE} debug ${LUAJIT_LIBRARY_DEBUG})

set(COMMON_DEFINITIONS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
set(COMMON_OPTIONS /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)

# What SacredBild and Sacred Gear share: the log, formatting, code patches and hooks, signature search. Each DLL links
# its own copy (its own log file, its own Detours transactions).
add_library(sacred_core STATIC
  src/fmt.cpp
  src/instance.cpp
  src/log.cpp
  src/patch.cpp
  src/sig.cpp
)
target_include_directories(sacred_core PUBLIC src PRIVATE ${DETOURS_INCLUDE_DIRS})
target_link_libraries(sacred_core PUBLIC ${DETOURS_LIBRARY})
target_compile_definitions(sacred_core PUBLIC ${COMMON_DEFINITIONS})
target_compile_options(sacred_core PRIVATE ${COMMON_OPTIONS})

# Largest sources first: MSBuild (/MP) and Ninja start compiles in list order, so a big file listed late would
# finish alone at the end of a fresh build.
function(sort_sources_by_size_desc var)
  set(sized)
  set(rest)
  foreach(src IN LISTS ${var})
    if(src MATCHES "\\.cpp$")
      file(SIZE "${CMAKE_CURRENT_SOURCE_DIR}/${src}" size)
      list(APPEND sized "${size}|${src}")
    else()
      list(APPEND rest "${src}")
    endif()
  endforeach()
  list(SORT sized COMPARE NATURAL ORDER DESCENDING)
  list(TRANSFORM sized REPLACE "^[0-9]+\\|" "")
  set(${var} ${sized} ${rest} PARENT_SCOPE)
endfunction()

set(DDRAW_SOURCES
  src/main.cpp
  src/proxy.cpp
  src/config.cpp
  src/crash_dump.cpp
  src/profiler.cpp
  src/settings_window.cpp
  src/system_ddraw.cpp
  src/game/build.cpp
  src/game/focus.cpp
  src/game/window_fit.cpp
  src/game/frame_hooks.cpp
  src/game/gameserver.cpp
  src/game/granny_async.cpp
  src/game/granny_parallel.cpp
  src/game/language.cpp
  src/game/map_cache.cpp
  src/game/sound_lock.cpp
  src/game/fast_math.cpp
  src/game/file_io_stats.cpp
  src/game/file_cache.cpp
  src/game/ground_quads.cpp
  src/game/sprite_quads.cpp
  src/game/ground_mesh.cpp
  src/game/layout_check.cpp
  src/game/skin_check.cpp
  src/game/granny_mesh.cpp
  src/game/gpu_skin.cpp
  src/game/world_passes.cpp
  src/game/aim_assist.cpp
  src/game/controller.cpp
  src/game/hero_move.cpp
  src/game/click_walk.cpp
  src/game/zoom.cpp
  src/game/options_screen.cpp
  src/game/ui_nav.cpp
  src/game/gear_nav.cpp
  src/game/ui_state.cpp
  src/game/ui_windows.cpp
  src/game/movie.cpp
  src/game/d3d_stats.cpp
  src/game/device_proxy.cpp
  src/game/resolution.cpp
  src/game/screenshot.cpp
  src/game/ui_anchor.cpp
  src/game/ui_canvas.cpp
  src/input/bindings.cpp
  src/input/gamepad.cpp
  src/input/inject.cpp
  src/input/input_mode.cpp
  src/overlay/overlay.cpp
  src/overlay/prompts.cpp
  src/ddraw9/device.cpp
  src/ddraw9/device_skin.cpp
  src/ddraw9/device_ground.cpp
  src/ddraw9/smaa.cpp
  src/ddraw9/directdraw.cpp
  src/ddraw9/format.cpp
  src/ddraw9/gpu.cpp
  src/ddraw9/surface.cpp
  src/ddraw9/vertex_buffer.cpp
  src/render/atlas.cpp
  src/render/batcher.cpp
  src/ui/form.cpp
  src/ui/form.rc
  src/net/address.cpp
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
sort_sources_by_size_desc(DDRAW_SOURCES)
# Version resources: version_info.h comes from the version files plus the git short sha (and -dirty), regenerated on every
# build (cmake/VersionInfo.cmake), and is only touched when its text changes.
set(VERSION_INFO_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(version_info ALL
  COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${VERSION_INFO_DIR}/version_info.h -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VersionInfo.cmake
  BYPRODUCTS ${VERSION_INFO_DIR}/version_info.h
  COMMENT "Generating version_info.h"
  VERBATIM)

add_library(ddraw SHARED ${DDRAW_SOURCES})
add_dependencies(ddraw version_info)
target_include_directories(ddraw PRIVATE ${VERSION_INFO_DIR})
target_include_directories(ddraw PRIVATE src ${DETOURS_INCLUDE_DIRS})
# The resource script embeds the button prompts' atlas (tools/gen_prompts.py).
set_source_files_properties(src/version.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/overlay/prompts.png;${VERSION_INFO_DIR}/version_info.h")
target_link_libraries(ddraw PRIVATE sacred_core ${DETOURS_LIBRARY} imgui::imgui kcp::kcp SDL3::SDL3-static psapi dxguid gdi32 ole32 oleaut32 mfplat mfuuid ws2_32 iphlpapi comctl32 delayimp)
target_compile_options(ddraw PRIVATE ${COMMON_OPTIONS})
target_precompile_headers(ddraw PRIVATE src/pch.h)
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

# Sacred Gear: the LuaJIT mod kit, a DLL of its own (docs/Gear.md). It does not need SacredBild: SacredBild loads it
# from the game folder, and any other DLL loader (an ASI loader, an injector) can load it instead.
option(SACREDGEAR "Build Sacred Gear (SacredGear.dll)" ON)
if(SACREDGEAR)
  add_library(SacredGear SHARED
    gear/main.cpp
    gear/hooks.cpp
    gear/lua_host.cpp
    gear/classes.cpp
    gear/classes_alias.cpp
    gear/itemuse.cpp
    gear/master.cpp
    gear/killstat.cpp
    gear/party.cpp
    gear/potionpick.cpp
    gear/buffs.cpp
    gear/pfx.cpp
    gear/tables.cpp
    gear/overlay.cpp
    gear/sitehook.cpp
    gear/world.cpp
    gear/pipe.cpp
    gear/automation.cpp
    gear/ui.cpp
    gear/padnav.cpp
    gear/text.cpp
    gear/pickup.cpp
    gear/drops.cpp
    gear/netmsg.cpp
    gear/chat.cpp
    gear/inventory.cpp
    gear/itemtip.cpp
    gear/save.cpp
    gear/savegames.cpp
    gear/replace.cpp
    gear/gamecfg.cpp
    gear/keyblock.cpp
    gear/input.cpp
    gear/itemicon.cpp
    gear/realm.cpp
    gear/rml.cpp
    gear/gear.rc
  )
  add_dependencies(SacredGear version_info)
  target_include_directories(SacredGear PRIVATE ${VERSION_INFO_DIR} gear ${LUAJIT_INCLUDE_DIRS}/luajit ${DETOURS_INCLUDE_DIRS} ${STB_INCLUDE_DIRS})
  set_source_files_properties(gear/gear.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gear/lua/kernel.lua;${CMAKE_CURRENT_SOURCE_DIR}/gear/lua/modmanager.lua;${CMAKE_CURRENT_SOURCE_DIR}/gear/lua/lookup.lua;${CMAKE_CURRENT_SOURCE_DIR}/gear/lua/fmt.lua;${CMAKE_CURRENT_SOURCE_DIR}/gear/lua/toast.lua;${CMAKE_CURRENT_SOURCE_DIR}/gear/lua/store.lua;${CMAKE_CURRENT_SOURCE_DIR}/gear/lua/kit.lua;${CMAKE_CURRENT_SOURCE_DIR}/gear/lua/modmanager.rml;${VERSION_INFO_DIR}/version_info.h")
  target_link_libraries(SacredGear PRIVATE sacred_core ${LUAJIT_LIBRARY} RmlUi::RmlUi shell32)
  target_compile_definitions(SacredGear PRIVATE RMLUI_STATIC_LIB)
  target_compile_options(SacredGear PRIVATE ${COMMON_OPTIONS})
  # SAFESEH off as for ddraw (Detours trampolines); /MAP for profiles.
  target_link_options(SacredGear PRIVATE /SAFESEH:NO /MAP)
  set_target_properties(SacredGear PROPERTIES OUTPUT_NAME "SacredGear")
  if(SACRED_GAME_DIR)
    add_custom_command(TARGET SacredGear POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SacredGear> "${SACRED_GAME_DIR}/SacredGear.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:SacredGear> "${SACRED_GAME_DIR}/SacredGear.pdb")
  endif()
endif()

# Test bench for the UDP transport and the matchmaker client outside the game (tools/nettest/nettest.cpp).
option(SACREDBILD_NETTEST "Build tools/nettest" OFF)
if(SACREDBILD_NETTEST)
  add_executable(nettest
    tools/nettest/nettest.cpp
    src/config.cpp
    src/fmt.cpp
    src/log.cpp
    src/net/address.cpp
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
  target_compile_options(nettest PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
  target_precompile_headers(nettest PRIVATE src/pch.h)
endif()
