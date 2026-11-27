# The vendored libraries (third_party/README.md): NVRHI with its D3D12 and Vulkan backends, static, for ddraw.dll.

# DirectX-Headers and Vulkan-Headers under the target names NVRHI's CMake looks for (their own CMake files are not vendored).
add_library(DirectX-Headers INTERFACE)
target_include_directories(DirectX-Headers SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/DirectX-Headers/include
  ${CMAKE_CURRENT_SOURCE_DIR}/DirectX-Headers/include/directx)
add_library(Microsoft::DirectX-Headers ALIAS DirectX-Headers)

add_library(DirectX-Guids STATIC DirectX-Headers/src/dxguids.cpp)
target_link_libraries(DirectX-Guids PRIVATE DirectX-Headers)
add_library(Microsoft::DirectX-Guids ALIAS DirectX-Guids)

add_library(Vulkan-Headers INTERFACE)
target_include_directories(Vulkan-Headers SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers/include)
# vulkan-1.dll is never linked: SacredBild loads it when the Vulkan API is chosen and hands its vkGetInstanceProcAddr to
# Vulkan-Hpp's dispatcher (the one NVRHI's backend calls through). Every user of vulkan.hpp must agree on this.
target_compile_definitions(Vulkan-Headers INTERFACE VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 VK_USE_PLATFORM_WIN32_KHR)
add_library(Vulkan::Headers ALIAS Vulkan-Headers)

set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_DIRECTX_HEADERS OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(nvrhi)

# Warnings are upstream's business; ours stay at /W4 for src/.
foreach(t nvrhi nvrhi_d3d12 nvrhi_vk DirectX-Guids)
  target_compile_options(${t} PRIVATE /W0 /utf-8)
  set_target_properties(${t} PROPERTIES FOLDER "third_party")
endforeach()
