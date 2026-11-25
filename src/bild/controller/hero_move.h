#pragma once

// Walking by the game's own move orders instead of a held mouse button. A held button starts with a path-finding
// walk to the clicked cell's center for half a second before hold-to-walk follows the cursor, and once something
// blocks the hero's way he stands until the button is clicked again. Here the order is the follow-the-cursor walk
// hold-to-walk gives (the hero heads straight at the cursor, recomputed each step); it goes out again only once the
// hero stands still, since each order restarts the walk. That walk lasts only while the cursor's left button is held,
// so the cMouse's held bit is set with it (without a button press, whose click would start the path-finding walk)
// and cleared on stop. Orders go out on the window's thread, where the game's mouse handler sends its own.
namespace HeroMove
{
    // Walk toward the cursor, here (x, y) in client pixels (the walk follows the game's cursor from then on).
    void follow(int x, int y);
    // While the walk lasts, now and then: the order again if the hero has not moved since the last call (blocked, or
    // his walk ended).
    void keepWalking(int x, int y);
    // Stop where the hero stands, as letting go of hold-to-walk does.
    void stop();
    // Just let go of the held bit: another order (an attack's click) takes over.
    void release();
}
