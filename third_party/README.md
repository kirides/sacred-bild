# Vendored libraries

Copied in, not fetched at build time, so a build needs no network and every version is pinned here. Update by copying the
new upstream files over the folder, then re-applying the local changes listed below (the second commit of each update).

| Folder | Upstream | Version | What is kept | License |
|---|---|---|---|---|
| `nvrhi/` | https://github.com/NVIDIA-RTX/NVRHI | `648f745142a929a1a61c91799a3be1f6969bbcf8` (main, 2026-11-27) | `CMakeLists.txt`, `cmake/`, `include/`, `src/`, `tools/` (natvis), licenses, README | MIT (`nvrhi/LICENSE.txt`) |
| `DirectX-Headers/` | https://github.com/microsoft/DirectX-Headers | `v1.717.0-preview` (`d873b34`), the tag NVRHI's CMake pins | `include/` without the `.idl` files, `src/`, license, README | MIT (`DirectX-Headers/LICENSE`) |
| `Vulkan-Headers/` | https://github.com/KhronosGroup/Vulkan-Headers | `v1.4.352` (`015e25c`), the tag NVRHI's CMake pins | `include/`, licenses, README | Apache-2.0 / MIT (`Vulkan-Headers/LICENSE.md`) |

`DirectX-Headers/` and `Vulkan-Headers/` are built by `third_party/CMakeLists.txt` (their own CMake files are not kept): interface
targets with the include folders and `DirectX-Guids` from `src/dxguids.cpp`, under the names NVRHI looks for.

## Local changes to NVRHI

Only what a 32-bit (Win32) build needs; Sacred is a 32-bit process. Each change is marked `// sacred-bild: x86` in the source.

- `include/nvrhi/common/misc.h` `hash_to_u32`: `hash >> 32` on a 32-bit `size_t` is undefined; shifts the value widened to 64 bits.
- `src/d3d12/d3d12-backend.h`: `rtxmuId` initialised with `~size_t(0)` instead of `~0ull`.
- `src/d3d12/d3d12-buffer.cpp` map range: the `uint64_t` byte size narrowed to `SIZE_T` explicitly.
- `src/d3d12/d3d12-meshlets.cpp`: `SIZE_T` is `unsigned long` and `size_t` `unsigned int` on x86, so `getBytecode` writes into a
  `size_t` that is then copied into the `BytecodeLength` fields.
- `src/d3d12/d3d12-raytracing.cpp`: the two `static_assert`s that a pointer is as large as the GPU address sharing its union
  are relaxed to `<=` (true on x86, the union is 8 bytes); the shader table's upload offset is a `size_t`, as
  `suballocateBuffer` takes it.
- `src/d3d12/d3d12-texture.cpp` and `src/d3d12/d3d12-upload.cpp`: `std::min`/`std::max`/`align` called with one explicit type
  where a `size_t` met a `uint64_t`.

Vulkan backend: in 32-bit builds Vulkan's non-dispatchable handles (`VkImage`, `VkBuffer`, `VkDeviceMemory`, ...) are
`uint64_t`, not pointers, and Vulkan-Hpp leaves out its handle classes' conversions to them.

- `CMakeLists.txt`: `VULKAN_HPP_TYPESAFE_CONVERSION=1` (public) for 32-bit builds, which restores those conversions as 64-bit
  builds have them by default.
- `src/vulkan/vulkan-backend.h`, `vulkan-device.cpp`: `nameVKObject` takes the handle as `uint64_t`; `VulkanContext::vkHandle`
  widens a C handle of either kind; the callers in `vulkan-buffer.cpp`, `vulkan-shader.cpp`, `vulkan-texture.cpp` and
  `vulkan-device.cpp` (and the buffer log line, which used `reinterpret_cast<uintptr_t>`) go through it.
- `src/vulkan/vulkan-backend.h`, `vulkan-allocator.cpp`: `VK_NULL_HANDLE` instead of `nullptr` for the dedicated image and
  buffer of `allocateMemory`.
- `include/nvrhi/common/resource.h` `Object(void*)`: zeroes the 8-byte union before storing a 4-byte pointer, so `integer`
  has no stale upper half (`getNativeObject` returns Vulkan handles in `integer` and interfaces in `pointer`).
