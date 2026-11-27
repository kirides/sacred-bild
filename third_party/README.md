# Vendored libraries

Copied in, not fetched at build time, so a build needs no network and every version is pinned here. Update by copying the
new upstream files over the folder, then re-applying the local changes listed below (the second commit of each update).

| Folder | Upstream | Version | What is kept | License |
|---|---|---|---|---|
| `nvrhi/` | https://github.com/NVIDIA-RTX/NVRHI | `648f745142a929a1a61c91799a3be1f6969bbcf8` (main, 2026-11-27) | `CMakeLists.txt`, `cmake/`, `include/`, `src/`, `tools/` (natvis), licenses, README | MIT (`nvrhi/LICENSE.txt`) |
| `DirectX-Headers/` | https://github.com/microsoft/DirectX-Headers | `v1.717.0-preview` (`d873b34`), the tag NVRHI's CMake pins | `include/` without the `.idl` files, `src/`, license, README | MIT (`DirectX-Headers/LICENSE`) |
| `Vulkan-Headers/` | https://github.com/KhronosGroup/Vulkan-Headers | `v1.4.352` (`015e25c`), the tag NVRHI's CMake pins | `include/`, licenses, README | Apache-2.0 / MIT (`Vulkan-Headers/LICENSE.md`) |
| `D3D12MemoryAllocator/` | https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator | `v3.3.0` (`6009492`) | `include/`, `src/D3D12MemAlloc.cpp` and `.natvis`, license, notices, README | MIT (`D3D12MemoryAllocator/LICENSE.txt`) |
| `VulkanMemoryAllocator/` | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | `v3.5.0` (`0f1a757`) | `include/vk_mem_alloc.h`, license, README | MIT (`VulkanMemoryAllocator/LICENSE.txt`) |
| `libdeflate/` | https://github.com/ebiggers/libdeflate | `1.26` (`68351dc`) | `libdeflate.h`, `common_defs.h`, the decompression sources of `lib/` (x86), `COPYING`, README | MIT (`libdeflate/COPYING`) |

`libdeflate/` is built by `third_party/CMakeLists.txt` as `libdeflate_decompress` (C11, MSVC's C11 atomics), linked into ddraw.dll for `perf/fast_inflate.cpp`; unmodified.

`D3D12MemoryAllocator/` is built by `third_party/CMakeLists.txt` as a static library (`D3D12MA`, with `D3D12MA_USING_DIRECTX_HEADERS`) linked into NVRHI's D3D12 backend; unmodified.

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

### Pipeline cache (`// sacred-bild: pipeline cache`)

- `include/nvrhi/d3d12.h` `DeviceDesc::pipelineLibrary` / `pipelineLibraryLock`, kept in the d3d12 `Context`
  (`d3d12-backend.h`, `d3d12-device.cpp`); `RootSignature::blobHash` (FNV-1a of the serialized root signature,
  `d3d12-resource-bindings.cpp`); `d3d12-graphics.cpp` loads a graphics pipeline state from the library by a name
  hashed from its description and stores the ones it makes.
- `include/nvrhi/vulkan.h` `DeviceDesc::pipelineCacheData` / `pipelineCacheSize` (initial data of the VkPipelineCache,
  an empty cache if the driver refuses it) and `nvrhi::vulkan::getPipelineCacheData`; `vulkan-backend.h`
  `Device::getContext`.

### Residency (`// sacred-bild: residency`)

- `d3d12-backend.h` / `d3d12-device.cpp`: the `Context` keeps `ID3D12Device3` and a residency fence; committed textures
  (`d3d12-texture.cpp`) are made with `D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT` and `EnqueueMakeResident` signals the fence;
  `executeCommandLists` makes the queue wait for its last value first. Making a texture no longer waits for its memory.

### Placed resources (`// sacred-bild: placed resources`)

- `include/nvrhi/nvrhi.h` `TextureDesc::sharesMemory` (opt-in: the contents are undefined until written; never for render
  targets or UAVs); `include/nvrhi/d3d12.h` `DeviceDesc::pAdapter` / `placedResources`.
- `d3d12-device.cpp` creates a D3D12 Memory Allocator (`Context::allocator`, default pools not zeroed) and releases it
  with the device; `d3d12-texture.cpp` makes such textures with `CreateResource3` / `CreateResource` placed in its heaps,
  the allocation owned by the texture (`PlacedAllocation`, released after the resource).
- `BufferDesc::sharesMemory` the same for GPU-only buffers (`d3d12-buffer.cpp`, Vulkan `allocateBufferMemory`); D3D12MA
  allocates from two pools of fixed-size heaps that stay (`Context::texturePool`, `bufferPool`).
- `include/nvrhi/vulkan.h` `DeviceDesc::placedResources`; `vulkan-allocator.cpp` compiles Vulkan Memory Allocator
  (`VMA_IMPLEMENTATION`, dynamic functions from `VULKAN_HPP_DEFAULT_DISPATCHER`), `createPlacedAllocator` /
  `destroyPlacedAllocator` (called by the device) and binds `sharesMemory` textures to its blocks
  (`MemoryResource::placed`; `allocateTextureMemory` / `freeTextureMemory`).

### Binding states (`// sacred-bild: binding states`)

- `d3d12-graphics.cpp` `setGraphicsState`: when a copy or write since the last draw left `m_BindingStatesDirty`, every
  binding set is updated (and its resources put back into their shader-resource state), as the Vulkan backend does;
  upstream only re-applied the states of sets that changed, so a texture rewritten between two draws with the same
  bindings was sampled in COPY_DEST.
