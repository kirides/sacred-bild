#include "game/controller.h"
#include "game/aim_assist.h"
#include "game/focus.h"
#include "game/frame_hooks.h"
#include "game/hero_move.h"
#include "game/resolution.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "game/ui_nav.h"
#include "input/bindings.h"
#include "input/gamepad.h"
#include "input/inject.h"
#include "input/input_mode.h"
#include "overlay/overlay.h"
#include "overlay/prompts.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace Sacred;
    using Bindings::Action;

    template <class T>
    T& member(void* obj, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }

    // Read by the window's thread (getClientCursorPos) as well.
    std::atomic<bool> g_drive{false};           // the controller has the cursor
    std::atomic<int64_t> g_cursor{0};           // client pixels, x << 32 | y
    std::atomic<bool> g_hideCursor{false};
    std::atomic<int64_t> g_override{0};         // clickAt's position ...
    std::atomic<DWORD> g_overrideUntil{0};      // ... until this tick
    std::atomic<bool> g_overrideOn{false};

    // The presenting thread's.
    float g_x = 0.0f, g_y = 0.0f;
    int64_t g_lastQpc = 0;
    int64_t g_qpcFrequency = 1;

    enum class Context
    {
        None,       // loading, cinematics, outside the game's windows
        Ui,         // menus and windows: the sticks move the cursor
        Game,       // walking and fighting
        Dialog,     // an NPC dialog with answers: A is Enter (the first answer), B clicks the second (Esc opens the
                    // game menu over it)
        Book,       // the log book: tabs, pages and list entries on buttons and sticks, no cursor
    };
    Context g_context = Context::None;
    bool g_cursorMode = false;                  // the Cursor action: windows' controls in game too

    Bindings::Resolver g_resolver;
    Bindings::Set g_held;                       // actions held in the last frame
    struct TapUp
    {
        int vk;
        int64_t due;
    };
    std::vector<TapUp> g_tapUp;                 // tapped keys, released when due

    // Game
    float g_faceX = 0.0f, g_faceY = 1.0f;       // the hero's last walking direction (screen)
    bool g_moving = false;
    int64_t g_orderQpc = 0;                     // the last move order or check (HeroMove)

    struct Attack
    {
        bool active = false;
        Action action = Action::Primary;
        int button = VK_LBUTTON;
        uint32_t target = 0;
        AimAssist::Kind kind = AimAssist::Kind::Enemy;
        bool down = false;      // the mouse button is down
        bool ctrl = false;      // no target: attack in place (Ctrl)
        bool done = false;      // a click on something to use went out; nothing more until released
    };
    Attack g_attack;

    // Menus and windows
    uint32_t g_repeatButton = 0;
    DWORD g_clickTick = 0;                      // the last A click, for double clicks
    float g_clickX = 0.0f, g_clickY = 0.0f;
    int64_t g_repeatQpc = 0;
    float g_wheel = 0.0f;

    constexpr float kRepeatDelay = 0.4f;        // D-pad: first repeat, then every
    constexpr float kRepeatInterval = 0.12f;
    constexpr float kNudge = 24.0f;             // D-pad step without a control that way, 1024x768 pixels
    constexpr float kWheelRate = 10.0f;         // wheel notches per second at full deflection
    constexpr float kStallCheck = 0.2f;         // walking: whether the hero still moves, this often
    constexpr uint32_t kUiButtons = Gamepad::A | Gamepad::B | Gamepad::X | Gamepad::Up | Gamepad::Down |
        Gamepad::Left | Gamepad::Right;
    constexpr uint32_t kBookButtons = Gamepad::B | Gamepad::LB | Gamepad::RB | Gamepad::Up | Gamepad::Down;

    // A held direction (stick or D-pad) as presses: one when pushed, more while held.
    struct Repeat
    {
        int direction = 0;
        int64_t due = 0;

        // -1 / 1 on a press or a repeat, else 0.
        int update(int held, int64_t tick)
        {
            if (held != direction)
            {
                direction = held;
                due = tick + static_cast<int64_t>(kRepeatDelay * g_qpcFrequency);
                return held;
            }
            if (held && tick >= due)
            {
                due = tick + static_cast<int64_t>(kRepeatInterval * g_qpcFrequency);
                return held;
            }
            return 0;
        }
    };
    Repeat g_bookSection, g_bookLeftPage, g_bookEntry, g_bookRightPage;   // the log book

    int64_t qpc()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    float seconds(int64_t ticks)
    {
        return static_cast<float>(ticks) / static_cast<float>(g_qpcFrequency);
    }

    int64_t pack(int x, int y)
    {
        return (static_cast<int64_t>(x) << 32) | static_cast<uint32_t>(y);
    }

    POINT unpack(int64_t v)
    {
        return {static_cast<LONG>(v >> 32), static_cast<LONG>(static_cast<int32_t>(v & 0xFFFFFFFF))};
    }

    float uiScale()
    {
        return Resolution::height() / 768.0f;
    }

    // Moves the controller's cursor; the game hears of it through a mouse move.
    void setCursor(float x, float y)
    {
        g_x = std::clamp(x, 0.0f, static_cast<float>(Resolution::width() - 1));
        g_y = std::clamp(y, 0.0f, static_cast<float>(Resolution::height() - 1));
        const int64_t p = pack(static_cast<int>(std::lround(g_x)), static_cast<int>(std::lround(g_y)));
        if (g_cursor.exchange(p) != p)
        {
            Inject::mouseMove();
        }
    }

    // Pressed now, released 80 ms later like a key press: the game polls some keys (Enter in NPC dialogs) at its own
    // pace and can miss a key that is down for a single frame.
    void tap(int vk)
    {
        Inject::key(vk, true);
        g_tapUp.push_back({vk, qpc() + g_qpcFrequency * 80 / 1000});
    }

    // An action's key for a frame: tapped on the press, held with the button, or its character on the press.
    void pressKey(const Bindings::Info& info, bool held, bool pressed)
    {
        switch (info.press)
        {
        case Bindings::Press::Hold:
            if (info.vk == VK_MENU)
            {
                Inject::hold(VK_MENU, held);    // its key message would open the window menu
            }
            else
            {
                Inject::key(info.vk, held);
            }
            break;
        case Bindings::Press::Toggle:
            if (pressed)
            {
                Inject::key(info.vk, !Inject::held(info.vk));
            }
            break;
        default:
            if (pressed)
            {
                tap(info.vk);
            }
            break;
        }
    }

    // Lets go of everything the controller holds in the game.
    void releaseAll()
    {
        Inject::releaseAll();
        g_tapUp.clear();
        g_attack = {};
        if (g_moving)
        {
            HeroMove::stop();
            g_moving = false;
        }
        g_repeatButton = 0;
        g_wheel = 0.0f;
        g_bookSection = g_bookLeftPage = g_bookEntry = g_bookRightPage = {};
        AimAssist::pickGame();
    }

    // The savegame window the controller saw open last (snapToNewestSavegame).
    void* g_savegames = nullptr;

    void start()
    {
        g_savegames = UiNav::savegames();   // what is open was the mouse's to pick from
        POINT p = {};
        GetCursorPos(&p);
        if (HWND window = Focus::window())
        {
            ScreenToClient(window, &p);
        }
        g_x = static_cast<float>(p.x);
        g_y = static_cast<float>(p.y);
        g_cursor = pack(p.x, p.y);
        g_drive = true;
    }

    void stop()
    {
        releaseAll();
        g_drive = false;
        g_hideCursor = false;
        g_context = Context::None;
        g_cursorMode = false;
    }

    bool shown(void* window)
    {
        return window && (member<uint32_t>(window, UiControl::flags) & 1);
    }

    // A modal window that just opened (message box, game menu) gets the cursor on its OK or first entry: A answers
    // it, B (Esc) cancels.
    void* g_modal = nullptr;

    void snapToModal()
    {
        void* m = UiNav::modal();
        if (m == g_modal)
        {
            return;
        }
        g_modal = m;
        float x, y;
        if (m && UiNav::home(m, x, y))
        {
            setCursor(x, y);
        }
    }

    // The load screen that just opened selects its newest savegame, with the cursor on it. Its list is filled and
    // its rows are selected on the window's thread, so the newest one is looked for there.
    std::atomic<int> g_newestRow{-1};

    void selectNewest(void*)
    {
        g_newestRow = UiNav::selectNewestSavegame();
    }

    void snapToNewestSavegame()
    {
        void* window = UiNav::savegames();
        if (window != g_savegames)
        {
            g_savegames = window;
            g_newestRow = -1;
            if (window)
            {
                Inject::call(&selectNewest, nullptr);
            }
        }
        float x, y;
        if (const int row = g_newestRow.exchange(-1); row >= 0 && window && UiNav::savegameRowPoint(row, x, y))
        {
            setCursor(x, y);
        }
    }

    // A window that takes the cursor is open (not the HUD: taskbar, minimap, overview map, chat, party portraits).
    bool windowOpen(void* manager)
    {
        for (uintptr_t offset = UiManager::firstGameWindow + 4; offset <= UiManager::lastGameWindow; offset += 4)
        {
            if (offset == UiManager::overviewMap || offset == UiManager::minimap || offset == UiManager::console ||
                offset == UiManager::netPortraits)
            {
                continue;
            }
            if (shown(member<void*>(manager, offset)))
            {
                return true;
            }
        }
        return UiNav::modal() != nullptr;
    }

    Context detect()
    {
        void* manager = *reinterpret_cast<void**>(Addr::g_pUiManager);
        if (!manager)
        {
            return Context::None;
        }
        const uint32_t flags = member<uint32_t>(manager, UiManager::flags);
        if (flags & 0x10)
        {
            return Context::None;   // cinematic
        }
        if (void* engine = FrameHooks::engine(); engine && (member<uint32_t>(engine, Engine::flags) & 0x10000))
        {
            return Context::None;   // loading
        }
        const bool inGame = (flags & UiManager::inGame) && !(flags & 0x01);
        if (inGame && UiNav::npcDialogOpen() && !UiNav::modal())
        {
            return Context::Dialog;
        }
        if (inGame && !g_cursorMode && UiNav::logBook() && !UiNav::modal())
        {
            return Context::Book;
        }
        float hx, hy;
        if (!inGame || g_cursorMode || windowOpen(manager) || !AimAssist::hero(hx, hy))
        {
            return Context::Ui;
        }
        return Context::Game;
    }

    // Diagnostics, the first changes of a session: what the UI manager shows (windows by slot and class, popups).
    void traceUi(void* manager)
    {
        static std::string last;
        static int lines = 0;
        if (lines >= 100)
        {
            return;
        }
        std::string shown;
        const auto add = [&](uintptr_t offset) {
            void* window = member<void*>(manager, offset);
            if (window && (member<uint32_t>(window, UiControl::flags) & 1))
            {
                shown += std::format("{}@{:x} ", UiNav::className(window), offset);
            }
        };
        for (uintptr_t offset = UiManager::firstGameWindow; offset <= UiManager::lastGameWindow; offset += 4)
        {
            add(offset);
        }
        for (uintptr_t offset = UiManagerMenus::first; offset <= UiManagerMenus::dialog; offset += 4)
        {
            add(offset);
        }
        add(0x14C);
        if (void* box = member<void*>(manager, UiManagerMenus::dialog); box && (member<uint32_t>(box, UiControl::flags) & 1))
        {
            shown += std::format("[{} buttons] ", UiNav::buttonCount(box));
        }
        void** popup = member<void**>(manager, UiManager::popupsBegin);
        void** end = member<void**>(manager, UiManager::popupsEnd);
        for (; popup && popup < end && end - popup < 256; ++popup)
        {
            if (*popup && (member<uint32_t>(*popup, UiControl::flags) & 1))
            {
                shown += "popup:" + UiNav::className(*popup) + "{" + UiNav::contents(*popup) + "} ";
            }
        }
        shown += std::format("| flags {:x}", member<uint32_t>(manager, UiManager::flags));
        if (shown != last)
        {
            last = shown;
            ++lines;
            LOG("Controller: UI shows {}", shown);
        }
    }

    // ---- Menus and windows ----

    // Window keys bound to buttons that the context leaves free (Start: Esc, Back: inventory, ...).
    void windowKeys(const Bindings::Set& pressed, uint32_t taken)
    {
        for (int i = 0; i < Bindings::actionCount; ++i)
        {
            const Bindings::Info& info = Bindings::info(static_cast<Action>(i));
            const Bindings::Binding b = Bindings::get(static_cast<Action>(i));
            if (pressed[i] && info.vk && std::string_view(info.group) == "Windows" && !(b.button & taken))
            {
                pressKey(info, false, true);
            }
        }
    }

    void click(float x, float y)
    {
        Controller::clickAt(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
    }

    // On the window's thread: scrolls the savegame list (high half, -1: not) and selects a row (low half).
    void selectSavegame(void* rowAndScroll)
    {
        const auto packed = static_cast<int32_t>(reinterpret_cast<intptr_t>(rowAndScroll));
        UiNav::selectSavegame(static_cast<int16_t>(packed >> 16), static_cast<int16_t>(packed & 0xFFFF));
    }

    void selectSavegame(int scrollTo, int row)
    {
        Inject::call(&selectSavegame, reinterpret_cast<void*>(static_cast<intptr_t>((scrollTo << 16) | (row & 0xFFFF))));
    }

    void dpadStep(uint32_t button)
    {
        const float dx = button == Gamepad::Left ? -1.0f : button == Gamepad::Right ? 1.0f : 0.0f;
        const float dy = button == Gamepad::Up ? -1.0f : button == Gamepad::Down ? 1.0f : 0.0f;
        float x, y;
        // Up / down in the savegame list: row by row, scrolling it at its ends; the row is selected.
        int row, scrollTo;
        if (dy != 0.0f && UiNav::savegameStep(g_x, g_y, static_cast<int>(dy), row, scrollTo))
        {
            if (row >= 0 && UiNav::savegameRowPoint(row, x, y))
            {
                selectSavegame(scrollTo, row);
                setCursor(x, y);
            }
            return;
        }
        if (UiNav::next(g_x, g_y, dx, dy, x, y))
        {
            setCursor(x, y);
            if (const int onRow = UiNav::savegameRow(x, y); onRow >= 0)
            {
                selectSavegame(-1, onRow);
            }
        }
        else
        {
            setCursor(g_x + dx * kNudge * uiScale(), g_y + dy * kNudge * uiScale());
        }
    }

    void uiFrame(float dt, const Bindings::Set& pressed)
    {
        const Gamepad::State& s = Gamepad::state();
        // Left stick: the cursor, slow near the center for aiming at small things.
        const float magnitude = std::sqrt(s.lx * s.lx + s.ly * s.ly);
        if (magnitude > 0.0f)
        {
            const float speed = g_config.controllerCursorSpeed * uiScale() * magnitude * dt;
            setCursor(g_x + s.lx * speed, g_y - s.ly * speed);
        }
        // D-pad: to the next control that way, repeating while held.
        const int64_t now = qpc();
        const uint32_t dpad = s.buttons & (Gamepad::Up | Gamepad::Down | Gamepad::Left | Gamepad::Right);
        const uint32_t dpadPressed = Gamepad::pressed() & dpad;
        if (dpadPressed)
        {
            g_repeatButton = dpadPressed & (~dpadPressed + 1);
            g_repeatQpc = now + static_cast<int64_t>(kRepeatDelay * g_qpcFrequency);
            dpadStep(g_repeatButton);
        }
        else if (g_repeatButton && (dpad & g_repeatButton))
        {
            if (now >= g_repeatQpc)
            {
                g_repeatQpc = now + static_cast<int64_t>(kRepeatInterval * g_qpcFrequency);
                dpadStep(g_repeatButton);
            }
        }
        else
        {
            g_repeatButton = 0;
        }
        // A / X: left / right mouse button (held: drag), B: Esc. A second A within the double-click time and
        // distance is a double click, as Windows makes of the mouse's (loading a savegame, ...); a third starts over.
        const bool aDown = (s.buttons & Gamepad::A) != 0;
        if (aDown && !Inject::held(VK_LBUTTON))
        {
            const DWORD tick = GetTickCount();
            const bool doubleClick = tick - g_clickTick <= GetDoubleClickTime() &&
                std::fabs(g_x - g_clickX) * 2.0f <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                std::fabs(g_y - g_clickY) * 2.0f <= GetSystemMetrics(SM_CYDOUBLECLK);
            Inject::button(VK_LBUTTON, true, doubleClick);
            g_clickTick = doubleClick ? tick - GetDoubleClickTime() - 1 : tick;
            g_clickX = g_x;
            g_clickY = g_y;
        }
        else if (!aDown)
        {
            Inject::button(VK_LBUTTON, false);
        }
        Inject::button(VK_RBUTTON, (s.buttons & Gamepad::X) != 0);
        // LB / RB: the inventory's previous / next page.
        if (const uint32_t shoulder = Gamepad::pressed() & (Gamepad::LB | Gamepad::RB); shoulder && !UiNav::modal())
        {
            float x, y;
            if (UiNav::inventoryTab((shoulder & Gamepad::RB) ? 1 : -1, x, y))
            {
                click(x, y);
            }
        }
        if (Gamepad::pressed() & Gamepad::B)
        {
            // A message box's Cancel (Esc cancels few of them), else Esc.
            float x, y;
            if (UiNav::cancelButton(x, y))
            {
                click(x, y);
            }
            else
            {
                tap(VK_ESCAPE);
            }
        }
        // Right stick: the mouse wheel (lists, zoom).
        if (s.ry == 0.0f)
        {
            g_wheel = 0.0f;
        }
        g_wheel += s.ry * kWheelRate * dt;
        for (; g_wheel >= 1.0f; g_wheel -= 1.0f)
        {
            Inject::wheel(WHEEL_DELTA);
        }
        for (; g_wheel <= -1.0f; g_wheel += 1.0f)
        {
            Inject::wheel(-WHEEL_DELTA);
        }
        windowKeys(pressed, kUiButtons);
    }

    // ---- The log book ----

    int axis(float v)
    {
        return v >= 0.5f ? 1 : v <= -0.5f ? -1 : 0;
    }

    // On the window's thread: the list's hit test measures its texts as the game's own clicks do.
    void stepEntry(void* step)
    {
        float x, y;
        if (UiNav::bookEntry(static_cast<int>(reinterpret_cast<intptr_t>(step)), x, y))
        {
            click(x, y);
        }
    }

    // LB / RB: the tabs across the top; D-pad up / down: the tabs down the left edge; left stick up / down: the list's
    // entries, left / right: its pages; right stick: the right page's pages (right or down: the next); B: Esc. Each
    // clicks the game's own control.
    void bookFrame(const Bindings::Set& pressed)
    {
        const Gamepad::State& s = Gamepad::state();
        const uint32_t down = Gamepad::pressed();
        const int64_t now = qpc();
        const bool leftAcross = std::fabs(s.lx) >= std::fabs(s.ly);
        const int section = g_bookSection.update((s.buttons & Gamepad::Down) ? 1 : (s.buttons & Gamepad::Up) ? -1 : 0, now);
        const int leftPage = g_bookLeftPage.update(leftAcross ? axis(s.lx) : 0, now);
        const int entry = g_bookEntry.update(leftAcross ? 0 : -axis(s.ly), now);
        const int rightPage = g_bookRightPage.update(axis(std::fabs(s.rx) >= std::fabs(s.ry) ? s.rx : -s.ry), now);
        float x = 0.0f, y = 0.0f;
        bool found = false;
        if (down & (Gamepad::LB | Gamepad::RB))
        {
            found = UiNav::bookTab((down & Gamepad::RB) ? 1 : -1, x, y);
        }
        else if (section)
        {
            found = UiNav::bookSection(section, x, y);
        }
        else if (leftPage)
        {
            found = UiNav::bookPage(false, leftPage, x, y);
        }
        else if (rightPage)
        {
            found = UiNav::bookPage(true, rightPage, x, y);
        }
        else if (entry)
        {
            Inject::call(&stepEntry, reinterpret_cast<void*>(static_cast<intptr_t>(entry)));
        }
        if (found)
        {
            click(x, y);
        }
        if (down & Gamepad::B)
        {
            tap(VK_ESCAPE);
        }
        windowKeys(pressed, kBookButtons);
    }

    // ---- Walking and fighting ----

    // A hit at or below the low-health mark (the portrait pulses below a quarter) rumbles, the stronger the less
    // health is left.
    constexpr float kLowHealth = 0.25f;
    int g_lastHealth = -1;

    void lowHealthRumble()
    {
        int health = 0, maximum = 0;
        if (!AimAssist::heroHealth(health, maximum) || maximum <= 0)
        {
            g_lastHealth = -1;
            return;
        }
        const float ratio = static_cast<float>(health) / static_cast<float>(maximum);
        if (g_lastHealth >= 0 && health < g_lastHealth && health > 0 && ratio < kLowHealth)
        {
            Gamepad::rumble(0.35f + 0.65f * (1.0f - ratio / kLowHealth), 220);
        }
        g_lastHealth = health;
    }

    // Where the hero stands on the screen: the center minus twice the view's offset (the camera trails the hero), the
    // point the game's own hold-to-walk measures the cursor from.
    void heroGround(float& x, float& y)
    {
        using InstanceFn = void*(__cdecl*)(int);
        using ViewOffsetFn = void(__fastcall*)(void* engine, void* edx, int* x, int* y);
        int ox = 0, oy = 0;
        if (void* engine = reinterpret_cast<InstanceFn>(Addr::cEngine_instance)(-1))
        {
            reinterpret_cast<ViewOffsetFn>(Addr::cEngine_getViewOffset)(engine, nullptr, &ox, &oy);
        }
        x = Resolution::width() * 0.5f - 2.0f * ox;
        y = Resolution::height() * 0.5f - 2.0f * oy;
    }

    // Ends the walk: the hero would go on following the cursor, so he gets the order to stop that letting go of
    // hold-to-walk gives. `order` false: an attack takes over and gives its own.
    void stopMoving(bool order)
    {
        if (!g_moving)
        {
            return;
        }
        g_moving = false;
        Inject::key(VK_SHIFT, false);
        if (order)
        {
            HeroMove::stop();
        }
        else
        {
            HeroMove::release();
        }
        AimAssist::pickGame();
    }

    // The cursor goes `radius` ahead of the hero and the hero follows it (HeroMove), in any direction to the pixel.
    // One order starts the walk; a hero something stopped gets it again, so he walks on once the way is free.
    void walk(float mx, float my, float magnitude)
    {
        if (magnitude <= 0.0f)
        {
            stopMoving(true);
            return;
        }
        const float dx = mx / magnitude, dy = my / magnitude;
        const float radius = static_cast<float>(g_config.controllerMoveRadius);
        AimAssist::pickNothing();   // the cursor ahead never highlights what it passes over
        Inject::key(VK_SHIFT, g_config.controllerWalk && magnitude < 0.5f);
        float gx, gy;
        heroGround(gx, gy);
        setCursor(gx + dx * radius, gy + dy * radius);
        const int64_t now = qpc();
        const int x = static_cast<int>(std::lround(g_x)), y = static_cast<int>(std::lround(g_y));
        if (!g_moving)
        {
            g_moving = true;
            HeroMove::follow(x, y);
        }
        else if (seconds(now - g_orderQpc) >= kStallCheck)
        {
            HeroMove::keepWalking(x, y);
        }
        else
        {
            return;
        }
        g_orderQpc = now;
    }

    void endAttack()
    {
        if (g_attack.down)
        {
            Inject::button(g_attack.button, false);
        }
        g_attack = {};
        AimAssist::pickGame();
    }

    // Aims the attack: the best enemy (for Primary, else something to use nearby), or in place toward `ax, ay`.
    void acquire(float ax, float ay, bool directed, float hx, float hy)
    {
        const float range = static_cast<float>(g_config.controllerAimRange);
        const float cone = static_cast<float>(g_config.controllerAimCone);
        const float dx = directed ? ax : 0.0f, dy = directed ? ay : 0.0f;
        AimAssist::Target t;
        if (AimAssist::find(AimAssist::Kind::Enemy, dx, dy, range, cone, t))
        {
            g_attack.kind = AimAssist::Kind::Enemy;
        }
        else if (g_attack.action == Action::Primary && !g_attack.target &&
            AimAssist::find(AimAssist::Kind::Interact, dx, dy, range * 0.5f, cone, t))
        {
            g_attack.kind = AimAssist::Kind::Interact;
        }
        else
        {
            g_attack.target = 0;
            g_attack.ctrl = g_attack.action == Action::Primary;
            AimAssist::pickNothing();
            const float reach = g_config.controllerMoveRadius * 0.6f;
            setCursor(hx + ax * reach, hy + ay * reach);
            return;
        }
        g_attack.target = t.id;
        g_attack.ctrl = false;
        AimAssist::pickTarget(t.id);
        setCursor(t.x, t.y);
    }

    void beginAttack(Action action, float ax, float ay, bool directed, float hx, float hy)
    {
        stopMoving(false);
        endAttack();
        g_attack.active = true;
        g_attack.action = action;
        g_attack.button = action == Action::Primary ? VK_LBUTTON : VK_RBUTTON;
        acquire(ax, ay, directed, hx, hy);
        // The button goes down next frame, once the cursor is on the target.
    }

    void updateAttack(float ax, float ay, bool directed, float hx, float hy)
    {
        if (g_attack.done)
        {
            return;
        }
        if (g_attack.target)
        {
            AimAssist::Target t;
            if (AimAssist::locate(g_attack.target, g_attack.kind, t))
            {
                setCursor(t.x, t.y);
            }
            else if (g_attack.kind == AimAssist::Kind::Interact)
            {
                // Picked up, opened, gone.
                if (g_attack.down)
                {
                    Inject::button(g_attack.button, false);
                    g_attack.down = false;
                }
                g_attack.done = true;
                return;
            }
            else
            {
                // Dead or gone: on to the next enemy.
                if (g_attack.down)
                {
                    Inject::button(g_attack.button, false);
                    g_attack.down = false;
                }
                acquire(ax, ay, directed, hx, hy);
                return;
            }
        }
        else
        {
            const float reach = g_config.controllerMoveRadius * 0.6f;
            setCursor(hx + ax * reach, hy + ay * reach);
        }
        if (!g_attack.down)
        {
            Inject::button(g_attack.button, true);
            g_attack.down = true;
        }
        else if (g_attack.kind == AimAssist::Kind::Interact && g_attack.target)
        {
            // Talking, opening and picking up take a click, not a held button.
            Inject::button(g_attack.button, false);
            g_attack.down = false;
            g_attack.done = true;
        }
    }

    // At 1024x768 UiCanvas is off and leaves the cursor alone; then the controller hooks it itself.
    using GetClientCursorPosFn = void(__cdecl*)(HWND window, POINT* pt);
    using RenderCursorFn = void(__fastcall*)(void* self, void* edx, void* device, int flag);
    GetClientCursorPosFn g_origGetClientCursorPos = nullptr;
    RenderCursorFn g_origRenderCursor = nullptr;

    void __cdecl hookGetClientCursorPos(HWND window, POINT* pt)
    {
        POINT controller;
        if (pt && Controller::cursor(controller))
        {
            *pt = controller;
            return;
        }
        g_origGetClientCursorPos(window, pt);
    }

    void __fastcall hookRenderCursor(void* self, void* edx, void* device, int flag)
    {
        if (!Controller::hideGameCursor())
        {
            g_origRenderCursor(self, edx, device, flag);
        }
    }

    bool isAttack(Action a)
    {
        return a == Action::Primary || a == Action::Secondary || (a >= Action::Art1 && a <= Action::Art5);
    }

    void gameFrame(const Bindings::Set& held, const Bindings::Set& pressed)
    {
        const Gamepad::State& s = Gamepad::state();
        float hx, hy;
        AimAssist::hero(hx, hy);
        const float mx = s.lx, my = -s.ly;
        const float magnitude = std::sqrt(mx * mx + my * my);
        if (magnitude > 0.0f)
        {
            g_faceX = mx / magnitude;
            g_faceY = my / magnitude;
        }
        // Aim: the right stick if pushed, else the walking direction.
        float ax = g_faceX, ay = g_faceY;
        const float rightMagnitude = std::sqrt(s.rx * s.rx + s.ry * s.ry);
        if (rightMagnitude > 0.3f)
        {
            ax = s.rx / rightMagnitude;
            ay = -s.ry / rightMagnitude;
        }
        const bool directed = magnitude > 0.0f || rightMagnitude > 0.3f;

        // Keys: held ones follow their button, the others are tapped. Combat art slots select the art (6-0).
        for (int i = 0; i < Bindings::actionCount; ++i)
        {
            const auto action = static_cast<Action>(i);
            const Bindings::Info& info = Bindings::info(action);
            if (!info.vk || action == Action::StandStill)
            {
                continue;
            }
            pressKey(info, held[i], pressed[i]);
        }

        // Attacks: the newest button pressed wins; the attack lasts while its button is held.
        if (g_attack.active && !held[static_cast<int>(g_attack.action)])
        {
            endAttack();
        }
        for (int i = 0; i < Bindings::actionCount; ++i)
        {
            const auto action = static_cast<Action>(i);
            const bool art = action >= Action::Art1 && action <= Action::Art5;
            if (pressed[i] && isAttack(action) && (!art || g_config.controllerArtClick))
            {
                beginAttack(action, ax, ay, directed, hx, hy);
            }
        }
        if (g_attack.active)
        {
            updateAttack(ax, ay, directed, hx, hy);
        }
        else
        {
            walk(mx, my, magnitude);
        }
        Inject::key(VK_CONTROL, held[static_cast<int>(Action::StandStill)] || (g_attack.active && g_attack.ctrl));
    }
}

namespace
{
    // ---- Button prompts ----

    constexpr float kPromptSize = 20.0f;        // 1024x768 pixels, scaled like the UI
    constexpr float kPromptMargin = 4.0f;

    // Screen pixels per 1024x768 pixel.
    float canvasScale()
    {
        return (UiCanvas::toPhysicalY(768) - UiCanvas::toPhysicalY(0)) / 768.0f;
    }

    Bindings::Binding single(uint32_t button)
    {
        return {button, 0};
    }

    // `binding` left of `r` (right of it if there is no room), vertically centered.
    void promptBeside(Bindings::Binding binding, const UiNav::Rect& r)
    {
        const float scale = canvasScale();
        const float size = kPromptSize * scale, margin = kPromptMargin * scale;
        const float y = (r.top + r.bottom) * 0.5f;
        if (r.left - margin - Prompts::width(binding, size) >= 0.0f)
        {
            Prompts::add(binding, r.left - margin, y, size, 1.0f, 0.5f);
        }
        else
        {
            Prompts::add(binding, r.right + margin, y, size, 0.0f, 0.5f);
        }
    }

    // `binding` centered above the point (a tab's center).
    void promptAbove(Bindings::Binding binding, float x, float y)
    {
        const float scale = canvasScale();
        Prompts::add(binding, x, y - 10.0f * scale, kPromptSize * scale, 0.5f, 1.0f);
    }

    void uiPrompts()
    {
        UiNav::Rect r;
        if (UiNav::controlAt(g_x, g_y, r))
        {
            promptBeside(single(Gamepad::A), r);
        }
        float x, y;
        if (UiNav::cancelButton(x, y) && UiNav::controlAt(x, y, r))
        {
            promptBeside(single(Gamepad::B), r);
        }
        if (!UiNav::modal())
        {
            if (UiNav::inventoryTab(-1, x, y))
            {
                promptAbove(single(Gamepad::LB), x, y);
            }
            if (UiNav::inventoryTab(1, x, y))
            {
                promptAbove(single(Gamepad::RB), x, y);
            }
        }
    }

    void dialogPrompts()
    {
        UiNav::Rect r;
        if (UiNav::npcAnswerRect(0, r))
        {
            promptBeside(single(Gamepad::A), r);
        }
        if (UiNav::npcAnswerRect(1, r))
        {
            promptBeside(single(Gamepad::B), r);
        }
    }

    void bookPrompts()
    {
        float x, y;
        if (UiNav::bookTab(-1, x, y))
        {
            promptAbove(single(Gamepad::LB), x, y);
        }
        if (UiNav::bookTab(1, x, y))
        {
            promptAbove(single(Gamepad::RB), x, y);
        }
        const float scale = canvasScale();
        const float size = kPromptSize * scale, margin = kPromptMargin * scale;
        if (UiNav::bookSection(-1, x, y))
        {
            Prompts::add(single(Gamepad::Up), x - 12.0f * scale - margin, y, size, 1.0f, 0.5f);
        }
        if (UiNav::bookSection(1, x, y))
        {
            Prompts::add(single(Gamepad::Down), x - 12.0f * scale - margin, y, size, 1.0f, 0.5f);
        }
    }

    // The bindings of the HUD's slots while Show names is held or the help screen is up.
    void hudPrompts(const Bindings::Set& held)
    {
        void* manager = *reinterpret_cast<void**>(Addr::g_pUiManager);
        const bool help = manager && (member<uint32_t>(manager, UiManager::flags) & UiManager::helpScreen);
        if (!help && !held[static_cast<int>(Action::ShowItems)])
        {
            return;
        }
        void* taskbar = member<void*>(manager, UiManager::taskbar);
        if (!taskbar || !(member<uint32_t>(taskbar, UiControl::flags) & 1))
        {
            return;
        }
        const float scale = canvasScale();
        const float size = kPromptSize * scale, gap = 3.0f * scale;
        UiNav::Rect r;
        // Weapon and combat art slots: above each, side by side.
        for (int i = 0; i < Taskbar::slotCount; ++i)
        {
            const auto weapon = static_cast<Action>(static_cast<int>(Action::Weapon1) + i);
            const auto art = static_cast<Action>(static_cast<int>(Action::Art1) + i);
            for (const auto& [offset, action] : {std::pair{Taskbar::weaponSlots, weapon}, std::pair{Taskbar::artSlots, art}})
            {
                void* slot = member<void*>(taskbar, offset + i * 4);
                if (slot && UiNav::controlRect(slot, taskbar, r))
                {
                    Prompts::add(Bindings::get(action), (r.left + r.right) * 0.5f, r.top - gap, size, 0.5f, 1.0f);
                }
            }
        }
        // Potions (Space Q W E R): the buttons stand close together, so their bindings stand upright.
        constexpr Action kPotions[] = {Action::Heal, Action::UndeadDeath, Action::Mentor, Action::Antidote,
            Action::Concentration};
        for (int i = 0; i < Taskbar::slotCount; ++i)
        {
            auto* full = static_cast<uint8_t*>(taskbar) + Taskbar::potionButtons + i * Taskbar::potionButtonSize;
            auto* empty = static_cast<uint8_t*>(taskbar) + Taskbar::potionButtonsEmpty + i * Taskbar::potionButtonSize;
            if (UiNav::controlRect(full, taskbar, r) || UiNav::controlRect(empty, taskbar, r))
            {
                Prompts::addStacked(Bindings::get(kPotions[i]), (r.left + r.right) * 0.5f, r.top - gap, size * 0.8f);
            }
        }
    }

    void showPrompts(Context context, const Bindings::Set& held)
    {
        switch (context)
        {
        case Context::Ui:
            uiPrompts();
            break;
        case Context::Dialog:
            dialogPrompts();
            break;
        case Context::Book:
            bookPrompts();
            break;
        case Context::Game:
            hudPrompts(held);
            break;
        default:
            break;
        }
    }
}

void Controller::install()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFrequency = f.QuadPart;
    if (!g_config.controller)
    {
        LOG("Controller: off ([Controller] Enabled=0)");
        return;
    }
    Bindings::load(g_config.iniPath);
    AimAssist::install();
    if (!UiCanvas::enabled())
    {
        Patch::hook(g_origGetClientCursorPos, Addr::getClientCursorPos, &hookGetClientCursorPos, "getClientCursorPos");
        Patch::hook(g_origRenderCursor, Addr::cMouse_renderCursor, &hookRenderCursor, "cMouse::renderCursor");
    }
    LOG("Controller: on (deadzone {}%, cursor {} px/s, walk radius {} px, aim {} px / {} degrees)",
        g_config.controllerDeadzone, g_config.controllerCursorSpeed, g_config.controllerMoveRadius,
        g_config.controllerAimRange, g_config.controllerAimCone);
}

void Controller::onFrame()
{
    if (!g_config.controller)
    {
        return;
    }
    // Menus present from the UI thread, the game from the render thread; around loading both may.
    static std::mutex frameLock;
    const std::unique_lock lock(frameLock, std::try_to_lock);
    if (!lock)
    {
        return;
    }
    Gamepad::setDeadzone(g_config.controllerDeadzone / 100.0f);
    Gamepad::poll();
    // This frame's prompts (none unless the controller drives), drawn into the next.
    Prompts::begin(Gamepad::state().style);
    struct PromptsDone
    {
        ~PromptsDone() { Prompts::end(); }
    } promptsDone;
    const int64_t now = qpc();
    const float dt = g_lastQpc ? std::clamp(seconds(now - g_lastQpc), 0.0f, 0.1f) : 0.0f;
    g_lastQpc = now;

    std::erase_if(g_tapUp, [&](const TapUp& t) {
        if (now < t.due)
        {
            return false;
        }
        Inject::key(t.vk, false);
        return true;
    });

    const uint32_t buttons = Focus::foreground() ? Gamepad::state().buttons : 0;
    const Bindings::Set held = g_resolver.update(buttons);
    const Bindings::Set pressed = held & ~g_held;
    g_held = held;

    if (!Focus::foreground())
    {
        if (g_drive)
        {
            stop();
        }
        return;
    }
    if (Gamepad::active())
    {
        InputMode::controllerUsed();
    }
    if (!InputMode::controller())
    {
        if (g_drive)
        {
            stop();
        }
        return;
    }
    if (!g_drive)
    {
        start();
    }
    if (Overlay::isOpen())
    {
        // SacredBild's own screen has the pad.
        if (g_context != Context::None)
        {
            releaseAll();
            g_context = Context::None;
        }
        return;
    }
    if (pressed[static_cast<int>(Action::Cursor)])
    {
        g_cursorMode = !g_cursorMode;
    }
    if (void* manager = *reinterpret_cast<void**>(Addr::g_pUiManager))
    {
        traceUi(manager);
    }
    const Context context = detect();
    if (context != g_context)
    {
        releaseAll();
        g_context = context;
    }
    g_hideCursor = context == Context::Game || context == Context::Dialog || context == Context::Book;
    if (context != Context::None)
    {
        lowHealthRumble();
    }
    snapToNewestSavegame();
    if (context == Context::Ui)
    {
        snapToModal();
        uiFrame(dt, pressed);
    }
    else if (context == Context::Dialog)
    {
        if (Gamepad::pressed() & Gamepad::A)
        {
            tap(VK_RETURN);
        }
        float x, y;
        if ((Gamepad::pressed() & Gamepad::B) && UiNav::npcAnswer(1, x, y))
        {
            click(x, y);
        }
    }
    else if (context == Context::Book)
    {
        bookFrame(pressed);
    }
    else if (context == Context::Game)
    {
        gameFrame(held, pressed);
    }
    if (g_config.controllerPrompts)
    {
        showPrompts(context, held);
    }
}

bool Controller::cursor(POINT& client)
{
    if (g_overrideOn.load(std::memory_order_relaxed))
    {
        if (static_cast<int>(GetTickCount() - g_overrideUntil.load()) < 0)
        {
            client = unpack(g_override.load());
            return true;
        }
        g_overrideOn = false;
    }
    if (!g_drive.load(std::memory_order_relaxed))
    {
        return false;
    }
    client = unpack(g_cursor.load());
    return true;
}

bool Controller::ownsCursor()
{
    return g_drive.load(std::memory_order_relaxed) || g_overrideOn.load(std::memory_order_relaxed);
}

bool Controller::hideGameCursor()
{
    return Overlay::isOpen() || (g_drive.load(std::memory_order_relaxed) && g_hideCursor.load(std::memory_order_relaxed));
}

void Controller::clickAt(int x, int y)
{
    g_override = pack(x, y);
    g_overrideUntil = GetTickCount() + 250;
    g_overrideOn = true;
    Inject::mouseMove();
    Inject::button(VK_LBUTTON, true);
    Inject::button(VK_LBUTTON, false);
}
