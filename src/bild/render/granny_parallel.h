#pragma once

// [Render] AnimationThreads: the animation controls of GrannyAdvanceTime sampled on several threads.
//
// The advance (granny.dll, see docs/RE_NOTES.md "Animation advance") walks one list of animation controls, each of
// which samples its animation (or pose) and blends the result into the bones of its target skeleton; then it poses the
// skeletons. The control walk is most of its time (~2 of ~3 ms zoomed out at 2560x1440). Controls of different
// skeletons write disjoint bones and read shared animation data only, so the walk is split by target skeleton: each
// skeleton's controls stay on one thread in list order (blending is order dependent), the skeletons are spread over
// the threads. The only shared write on the way, the 16-bit reference counts a control's animation handle gets while
// it samples, is made atomic during that phase. Controls that expired are unlinked and deleted afterwards on the
// calling thread, as the original walk does. Posing stays as it is.
namespace GrannyParallel
{
    // Hooks granny.dll's control walk; call inside a Patch transaction.
    void install();
    // Once per presented frame: statistics with [Debug] D3DStats.
    void onFrame();
}
