#pragma once

// [Render] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct Render
    {
        // Texture memory the game may keep loaded, in MB; 0 = max(game's own value, 256). A zoomed-out view at a
        // high resolution shows far more different ground textures than the original 1024x768.
        int textureBudgetMB = 0;

        // World view draws: merge consecutive draws that end up with the same device state into one call.
        bool batch = true;
        // Batched draws skip Direct3D 7's software clipping of pretransformed vertices; the GPU clips them.
        bool batchNoClip = true;
        // Batched draws go through vertex buffers instead of user memory (saves copies in the runtime and driver).
        bool batchVertexBuffer = true;
        // 3D models (characters and their shadows) go through the batcher too: vertex buffers, merged where possible.
        bool batchModels = true;
        // The ground's quad batcher hands its textures and quads to the batcher in one call instead of three device calls.
        bool batchGround = true;
        // The sprite batcher (water tiles, objects) hands its texture and quads to the batcher in one call instead of
        // two device calls per flush.
        bool batchSprites = true;
        // The ground's tiles kept in vertex buffers per sector (built once as they come into view) and drawn by a vertex
        // shader in a few draws per frame, instead of the game rebuilding and drawing every tile and blend layer each
        // frame (Direct3D 9 backend only).
        bool groundMesh = true;
        // Characters skinned in a vertex shader instead of by Granny on the CPU (Direct3D 9 backend only).
        bool gpuSkinning = true;
        // Skeletons not drawn in the last frames are posed every Nth frame instead of every frame (1 = every frame).
        int offscreenPoses = 4;
        // GrannyAdvanceTime runs on a worker thread, overlapping the start of the frame.
        bool asyncAnimation = true;
        // Threads that sample Granny's animation controls (split by skeleton); 1 = Granny's own walk, 0 = automatic.
        int animationThreads = 0;
        // Hash index in front of the map data's record caches (std::map lookups per tile and object).
        bool recordIndex = true;
        // The sound system's lock as a user-mode lock instead of a kernel mutex (two system calls per sound command).
        bool soundLock = true;
        // Faster replacements for the game's x87 math (float -> integer conversion, the teleporter ripple's sin/cos).
        bool fastMath = true;
        // Read the game's pak files and music once in the background so the game's reads during play come from
        // Windows' file cache instead of the drive (~8 ms each from an idle SSD, on the render thread).
        bool warmFileCache = true;
        // Copy small textures into shared pages so draws with different textures can be merged as well.
        bool atlas = true;
        int atlasPageSize = 8192;       // texels per side, clamped to the device limit
        int atlasPages = 2;             // at most this many pages per texture format
        int atlasMaxTextureSize = 512;  // larger textures are used directly
    };
    inline Render render;
}
