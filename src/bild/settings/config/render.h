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
        // SMAA antialiasing of the world view, before the UI is drawn (Direct3D 9 backend only): 0 off, 1 low,
        // 2 medium, 3 high (SMAA's presets).
        int smaa = 0;
        // [DDraw] Backend=nvrhi: the world view drawn into a target of its own this many times the frame's size, then
        // scaled down into the frame before the interface (supersampling: smoother edges, sharper textures at a cost).
        // 1 = straight into the frame, as the other backends.
        float worldScale = 1.0f;
        // [DDraw] Backend=nvrhi: SacredBild draws the draw lists' static objects (buildings, trees, props) itself
        // instead of the game's code for them (render/world_statics.h).
        bool takeOverStatics = true;
        // [DDraw] Backend=nvrhi: Sacred Gear's UI draws with SacredBild's renderer directly (core/bild_gfx.h) instead
        // of Direct3D 7 calls through the device proxy.
        bool gearGfx = true;
        // [DDraw] Backend=nvrhi: particles (lit quads with lighting off, a world matrix each) moved into world space on
        // the CPU and merged into the model batch: one draw per effect instead of one per particle.
        bool batchParticles = true;
        // [DDraw] Backend=nvrhi: pretransformed draws with the same state as the one before are drawn as one (the
        // interface's quads and what the batcher leaves).
        bool mergeDraws = true;
        // [DDraw] Backend=nvrhi: the GPU pipelines kept between runs (SacredBild-pipelines-<api>.bin next to the game;
        // discarded when SacredBild, the GPU or its driver changed).
        bool pipelineCache = true;
        // [DDraw] Backend=nvrhi: the world's point-sampled 2D art (ground tiles, sprites) magnified with an edge-directed
        // filter in the manner of xBR (smooth diagonals instead of stair steps when zoomed in). Off: point sampling.
        bool spriteFilter = false;
        // Filtering of the game world's textures: 0 the game's own point sampling, 1 sharp bilinear (crisp texels, soft
        // edges; Direct3D 9 backend, point sampling elsewhere), 2 bilinear (smoother when zoomed in). The interface
        // follows [UI] LinearFilter.
        int textureFilter = 0;
        // Texture filter value the proxy hands the Direct3D 9 backend for "sharp bilinear" (outside Direct3D 7's range).
        static constexpr unsigned long sharpFilter = 0x100;
        // Skeletons not drawn in the last frames are posed every Nth frame instead of every frame (1 = every frame).
        int offscreenPoses = 4;
        // GrannyAdvanceTime runs on a worker thread, overlapping the start of the frame.
        bool asyncAnimation = true;
        // With AsyncAnimation and a frame cap (FpsLimit or VSync): the frame's animation advance starts after the frame's
        // interface instead of the start of the next frame, so it runs while the frame waits for its cap and the next
        // frame's models don't wait for it (poses one frame behind the game's clock).
        bool animationAhead = true;
        // With AnimationThreads > 1: the skeletons posed on those threads too, grouped by their root skeleton.
        bool parallelPosing = true;
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
