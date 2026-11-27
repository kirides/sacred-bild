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
# RmlUi (Sacred Gear's HTML/CSS-style mod UI, src/gear/ui/rml.cpp) and stb_image (the images its documents name).
find_package(RmlUi CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
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

# NVRHI and the D3D12 / Vulkan headers, vendored (third_party/README.md): the renderer of [DDraw] Backend=nvrhi (docs/NvrhiRenderer.md).
add_subdirectory(third_party)

set(COMMON_DEFINITIONS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
set(COMMON_OPTIONS /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)

# What SacredBild and Sacred Gear share: the log, formatting, code patches and hooks, signature search. Each DLL links
# its own copy (its own log file, its own Detours transactions).
add_library(sacred_core STATIC
  src/core/fmt.cpp
  src/core/instance.cpp
  src/core/log.cpp
  src/core/patch.cpp
  src/core/sig.cpp
)
target_include_directories(sacred_core PUBLIC src/core PRIVATE ${DETOURS_INCLUDE_DIRS})
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

# The NVRHI device and swap chain ([DDraw] Backend=nvrhi, docs/NvrhiRenderer.md); also built into tools/test/rhitest.
set(RHI_SOURCES
  src/bild/rhi/rhi.cpp
  src/bild/rhi/rhi_d3d12.cpp
  src/bild/rhi/rhi_vulkan.cpp
)
# DirectDraw / Direct3D 7 on the NVRHI device (stage 3 of docs/NvrhiRenderer.md).
set(DDRAWRHI_SOURCES
  src/bild/ddrawrhi/context.cpp
  src/bild/ddrawrhi/device.cpp
  src/bild/ddrawrhi/directdraw.cpp
  src/bild/ddrawrhi/ground.cpp
  src/bild/ddrawrhi/surface.cpp
  src/bild/ddrawrhi/vertex_buffer.cpp
)
# Direct3D 12 and Vulkan headers stay in these two files, away from the Direct3D 7 / 9 headers of the precompiled header.
set_source_files_properties(src/bild/rhi/rhi_d3d12.cpp src/bild/rhi/rhi_vulkan.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set(DDRAW_SOURCES
  src/bild/main.cpp
  src/bild/proxy.cpp
  src/bild/settings/config.cpp
  src/bild/crash_dump.cpp
  src/bild/diag/profiler.cpp
  src/bild/settings/settings_window.cpp
  src/bild/system_ddraw.cpp
  src/bild/engine/build.cpp
  src/bild/display/focus.cpp
  src/bild/display/window_fit.cpp
  src/bild/render/frame_hooks.cpp
  src/bild/net/gameserver.cpp
  src/bild/render/granny_async.cpp
  src/bild/render/granny_parallel.cpp
  src/bild/game/language.cpp
  src/bild/perf/map_cache.cpp
  src/bild/perf/sound_lock.cpp
  src/bild/perf/fast_math.cpp
  src/bild/diag/file_io_stats.cpp
  src/bild/perf/file_cache.cpp
  src/bild/render/ground_quads.cpp
  src/bild/render/sprite_quads.cpp
  src/bild/render/ground_mesh.cpp
  src/bild/diag/layout_check.cpp
  src/bild/diag/skin_check.cpp
  src/bild/render/granny_mesh.cpp
  src/bild/render/gpu_skin.cpp
  src/bild/diag/world_passes.cpp
  src/bild/controller/aim_assist.cpp
  src/bild/game/pets.cpp
  src/bild/controller/controller.cpp
  src/bild/controller/hero_move.cpp
  src/bild/controller/click_walk.cpp
  src/bild/display/zoom.cpp
  src/bild/controller/options_screen.cpp
  src/bild/controller/ui_nav.cpp
  src/bild/controller/gear_nav.cpp
  src/bild/ui/ui_state.cpp
  src/bild/ui/ui_windows.cpp
  src/bild/game/movie.cpp
  src/bild/diag/d3d_stats.cpp
  src/bild/render/device_proxy.cpp
  src/bild/display/resolution.cpp
  src/bild/game/screenshot.cpp
  src/bild/ui/ui_anchor.cpp
  src/bild/ui/ui_canvas.cpp
  src/bild/controller/bindings.cpp
  src/bild/controller/gamepad.cpp
  src/bild/controller/inject.cpp
  src/bild/controller/input_mode.cpp
  src/bild/overlay/overlay.cpp
  src/bild/controller/prompts.cpp
  src/bild/ddraw9/device.cpp
  src/bild/ddraw9/device_skin.cpp
  src/bild/ddraw9/device_ground.cpp
  src/bild/ddraw9/smaa.cpp
  src/bild/ddraw9/directdraw.cpp
  src/bild/ddraw9/format.cpp
  src/bild/ddraw9/gpu.cpp
  src/bild/ddraw9/surface.cpp
  src/bild/ddraw9/vertex_buffer.cpp
  src/bild/render/atlas.cpp
  src/bild/render/batcher.cpp
  src/bild/settings/form.cpp
  src/bild/settings/form.rc
  src/bild/net/address.cpp
  src/bild/net/adapters.cpp
  src/bild/net/connection.cpp
  src/bild/net/lan_client.cpp
  src/bild/net/lan_protocol.cpp
  src/bild/net/lan_server.cpp
  src/bild/net/matchmaker.cpp
  src/bild/net/tincat_shim.cpp
  src/bild/net/udp_endpoint.cpp
  src/bild/net/udp_protocol.cpp
  src/bild/net/udp_transport.cpp
  src/bild/exports.def
  src/bild/version.rc
  ${RHI_SOURCES}
  ${DDRAWRHI_SOURCES}
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
target_include_directories(ddraw PRIVATE src/bild ${DETOURS_INCLUDE_DIRS})
# The resource script embeds the button prompts' atlas (tools/re/gen_prompts.py).
set_source_files_properties(src/bild/version.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/bild/controller/prompts.png;${VERSION_INFO_DIR}/version_info.h")
target_link_libraries(ddraw PRIVATE sacred_core ${DETOURS_LIBRARY} imgui::imgui kcp::kcp SDL3::SDL3-static psapi dxguid gdi32 ole32 oleaut32 mfplat mfuuid ws2_32 iphlpapi comctl32 nvrhi_all delayimp)
target_compile_options(ddraw PRIVATE ${COMMON_OPTIONS})
target_precompile_headers(ddraw PRIVATE src/bild/pch.h)
# SAFESEH must stay off: Detours trampolines are not registered handlers.
# The map file lets tools/re/profile_report.py name functions in SacredBild's own samples.
target_link_options(ddraw PRIVATE /SAFESEH:NO /MAP)
# Winsock and the IP helper load on first use (LAN relay), Media Foundation with the first movie, Common Controls
# in the settings window (version 6 there, from its activation context).
target_link_options(ddraw PRIVATE /DELAYLOAD:ws2_32.dll /DELAYLOAD:iphlpapi.dll /DELAYLOAD:mfplat.dll /DELAYLOAD:comctl32.dll)
# Direct3D 12 and DXGI only with [DDraw] Backend=nvrhi (Api=d3d12): ddraw.dll still loads where they are missing (Windows 7).
target_link_options(ddraw PRIVATE /DELAYLOAD:d3d12.dll /DELAYLOAD:dxgi.dll)
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
  # Everything of Sacred Gear but its DllMain and resources, compiled once: SacredGear.dll links it, and so does the UI sandbox
  # (tools/sandbox, docs/Sandbox.md), which runs the same Lua host, RmlUi host and UI replay outside the game.
  add_library(gear_objs OBJECT
    src/gear/hooks.cpp
    src/gear/lua_host.cpp
    src/gear/tables_lua.cpp
    src/gear/save_lua.cpp
    src/gear/net_lua.cpp
    src/gear/files_lua.cpp
    src/gear/automation_lua.cpp
    src/gear/world/world_lua.cpp
    src/gear/world/party_lua.cpp
    src/gear/items/items_lua.cpp
    src/gear/ui/ui_lua.cpp
    src/gear/world/classes.cpp
    src/gear/world/classes_alias.cpp
    src/gear/items/itemuse.cpp
    src/gear/items/master.cpp
    src/gear/world/killstat.cpp
    src/gear/world/party.cpp
    src/gear/items/potionpick.cpp
    src/gear/items/shop.cpp
    src/gear/world/buffs.cpp
    src/gear/world/pfx.cpp
    src/gear/tables.cpp
    src/gear/overlay.cpp
    src/gear/sitehook.cpp
    src/gear/world/world.cpp
    src/gear/pipe.cpp
    src/gear/automation.cpp
    src/gear/ui/ui.cpp
    src/gear/ui/padnav.cpp
    src/gear/ui/text.cpp
    src/gear/items/pickup.cpp
    src/gear/items/drops.cpp
    src/gear/netmsg.cpp
    src/gear/chat.cpp
    src/gear/items/inventory.cpp
    src/gear/items/itemtip.cpp
    src/gear/save.cpp
    src/gear/savegames.cpp
    src/gear/ui/replace.cpp
    src/gear/gamecfg.cpp
    src/gear/ui/keyblock.cpp
    src/gear/ui/input.cpp
    src/gear/items/itemicon.cpp
    src/gear/realm.cpp
    src/gear/ui/rml.cpp
    src/gear/ui/dds.cpp
    src/gear/vfs.cpp
    src/gear/ui/css_compat.cpp
  )
  add_dependencies(gear_objs version_info)
  target_include_directories(gear_objs PUBLIC ${VERSION_INFO_DIR} src/gear ${LUAJIT_INCLUDE_DIRS}/luajit ${DETOURS_INCLUDE_DIRS} ${STB_INCLUDE_DIRS})
  target_link_libraries(gear_objs PUBLIC sacred_core ${LUAJIT_LIBRARY} RmlUi::RmlUi ZLIB::ZLIB shell32)
  target_compile_definitions(gear_objs PUBLIC RMLUI_STATIC_LIB SACREDGEAR_BUILD)
  target_compile_options(gear_objs PRIVATE ${COMMON_OPTIONS})
  add_library(SacredGear SHARED
    src/gear/main.cpp
    src/gear/gear.rc
  )
  add_dependencies(SacredGear version_info)
  target_include_directories(SacredGear PRIVATE ${VERSION_INFO_DIR} src/gear ${LUAJIT_INCLUDE_DIRS}/luajit ${DETOURS_INCLUDE_DIRS} ${STB_INCLUDE_DIRS})
  # The kernel: its parts (src/gear/lua/kernel.parts) joined into the one chunk gear.rc embeds. gear.rc depends on every file in
  # lua/ and res/, so a new embedded file needs only its line in gear.rc.
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/src/gear/lua/kernel.parts GEAR_KERNEL_PARTS REGEX "^[^#].*")
  list(TRANSFORM GEAR_KERNEL_PARTS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/src/gear/lua/)
  add_custom_command(OUTPUT ${VERSION_INFO_DIR}/gear_kernel.lua
    COMMAND ${CMAKE_COMMAND} -DLUA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src/gear/lua -DOUT=${VERSION_INFO_DIR}/gear_kernel.lua -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/JoinKernel.cmake
    DEPENDS ${GEAR_KERNEL_PARTS} ${CMAKE_CURRENT_SOURCE_DIR}/src/gear/lua/kernel.parts ${CMAKE_CURRENT_SOURCE_DIR}/cmake/JoinKernel.cmake
    COMMENT "Joining the Sacred Gear kernel"
    VERBATIM)
  add_custom_target(gear_kernel DEPENDS ${VERSION_INFO_DIR}/gear_kernel.lua)
  add_dependencies(SacredGear gear_kernel)
  set(GEAR_RC ${CMAKE_CURRENT_SOURCE_DIR}/src/gear/gear.rc)
  file(GLOB GEAR_EMBEDDED CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/gear/lua/* ${CMAKE_CURRENT_SOURCE_DIR}/src/gear/res/*)
  list(APPEND GEAR_EMBEDDED ${VERSION_INFO_DIR}/gear_kernel.lua ${VERSION_INFO_DIR}/version_info.h)
  set_source_files_properties(src/gear/gear.rc PROPERTIES OBJECT_DEPENDS "${GEAR_EMBEDDED}")
  target_link_libraries(SacredGear PRIVATE gear_objs sacred_core ${LUAJIT_LIBRARY} RmlUi::RmlUi ZLIB::ZLIB shell32)
  target_compile_definitions(SacredGear PRIVATE RMLUI_STATIC_LIB SACREDGEAR_BUILD)
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

# Test bench for the UDP transport and the matchmaker client outside the game (tools/test/nettest/nettest.cpp).
option(SACREDBILD_NETTEST "Build tools/test/nettest" OFF)
if(SACREDBILD_NETTEST)
  add_executable(nettest
    tools/test/nettest/nettest.cpp
    src/bild/settings/config.cpp
    src/core/fmt.cpp
    src/core/instance.cpp
    src/core/log.cpp
    src/bild/net/address.cpp
    src/bild/net/adapters.cpp
    src/bild/net/lan_protocol.cpp
    src/bild/net/matchmaker.cpp
    src/bild/net/udp_endpoint.cpp
    src/bild/net/udp_protocol.cpp
    src/bild/net/udp_transport.cpp
  )
  target_include_directories(nettest PRIVATE src/bild src/core)
  target_link_libraries(nettest PRIVATE kcp::kcp ws2_32 iphlpapi winmm)
  target_compile_definitions(nettest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_compile_options(nettest PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
  target_precompile_headers(nettest PRIVATE src/bild/pch.h)
endif()

# Test bench for src/bild/rhi outside the game: device, swap chain, clears and a resize on Direct3D 12 or Vulkan.
option(SACREDBILD_RHITEST "Build tools/test/rhitest" ON)
if(SACREDBILD_RHITEST)
  add_executable(rhitest tools/test/rhitest/rhitest.cpp ${RHI_SOURCES})
  target_include_directories(rhitest PRIVATE src/bild)
  target_link_libraries(rhitest PRIVATE sacred_core nvrhi_all delayimp)
  target_compile_options(rhitest PRIVATE ${COMMON_OPTIONS})
  target_link_options(rhitest PRIVATE /DELAYLOAD:d3d12.dll /DELAYLOAD:dxgi.dll)
endif()

# The UI sandbox (docs/Sandbox.md): Sacred Gear's Lua host, RmlUi host and UI replay outside the game, on SacredBild's DirectDraw /
# Direct3D 7 emulation (Direct3D 9Ex), the game's images read from the install's files. Window or off-screen, PNG snapshots, driver
# scripts and the differential RmlUi fuzzer (tools/sandbox/fuzz).
option(SACREDGEAR_SANDBOX "Build tools/sandbox (gear_sandbox.exe)" ON)
if(SACREDGEAR AND SACREDGEAR_SANDBOX)
  add_executable(gear_sandbox
    tools/sandbox/sandbox.cpp
    tools/sandbox/game_data.cpp
    src/bild/ddraw9/device.cpp
    src/bild/ddraw9/device_skin.cpp
    src/bild/ddraw9/device_ground.cpp
    src/bild/ddraw9/directdraw.cpp
    src/bild/ddraw9/format.cpp
    src/bild/ddraw9/gpu.cpp
    src/bild/ddraw9/smaa.cpp
    src/bild/ddraw9/surface.cpp
    src/bild/ddraw9/vertex_buffer.cpp
    ${RHI_SOURCES}
    ${DDRAWRHI_SOURCES}
    ${GEAR_RC}
  )
  add_dependencies(gear_sandbox version_info gear_kernel)
  target_include_directories(gear_sandbox PRIVATE ${VERSION_INFO_DIR} src/gear src/bild tools/sandbox ${STB_INCLUDE_DIRS})
  target_link_libraries(gear_sandbox PRIVATE gear_objs sacred_core ${LUAJIT_LIBRARY} RmlUi::RmlUi ZLIB::ZLIB shell32 dxguid gdi32 ole32 psapi nvrhi_all delayimp)
  target_link_options(gear_sandbox PRIVATE /DELAYLOAD:d3d12.dll /DELAYLOAD:dxgi.dll)
  target_compile_definitions(gear_sandbox PRIVATE RMLUI_STATIC_LIB)
  target_compile_options(gear_sandbox PRIVATE ${COMMON_OPTIONS})
  # SacredGear's sources read game addresses under fault guards and find none here (every address stays 0); the image sits high so that
  # nothing of the sandbox lies where Sacred.exe would.
  target_link_options(gear_sandbox PRIVATE /SAFESEH:NO /BASE:0x50000000 /SUBSYSTEM:CONSOLE)
endif()
