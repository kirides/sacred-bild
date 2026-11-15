#include "game/ui_state.h"

#include <iterator>

namespace
{
    using namespace Sacred;
    using W = cUI_Manager::Window;

    bool visible(const cUI_Control2* control)
    {
        return control && control->visible();
    }

    // The windows the manager draws alone, in the order its render tests them.
    constexpr W kExclusive[] = {W::savegame, W::options, W::character, W::megamap};

    // HUD windows: drawn around the world, they don't take the cursor away from it.
    constexpr W kHud[] = {W::taskbar, W::overviewMap, W::minimap, W::console, W::netPortraits};

    bool isHud(W w)
    {
        for (W hud : kHud)
        {
            if (hud == w)
            {
                return true;
            }
        }
        return false;
    }

    constexpr const char* kNames[] = {
        "taskbar", "inventory", "equipment", "blacksmith", "merchant", "megamap", "overviewMap", "minimap", "stats",
        "console", "questbook", "savegame", "escapeMenu", "purchase", "master", "options", "chest", "horse",
        "networkInfo", "netPortraits", "character", "cube", "trade",
    };
    static_assert(std::size(kNames) == static_cast<size_t>(W::count));
}

cUI_Manager* UiState::manager()
{
    return cUI_Manager::instance();
}

UiState::Mode UiState::mode()
{
    const cUI_Manager* m = manager();
    if (!m)
    {
        return Mode::None;
    }
    const uint32_t flags = m->flags;
    if (flags & cUI_Manager::cinematic)
    {
        return Mode::Cinematic;
    }
    if (flags & cUI_Manager::inMenus)
    {
        return Mode::Menus;
    }
    return flags & cUI_Manager::inGame ? Mode::Game : Mode::None;
}

cUI_Window2* UiState::window(Window w)
{
    cUI_Manager* m = manager();
    if (!m || !(m->flags & cUI_Manager::windowsCreated) || w >= Window::count)
    {
        return nullptr;
    }
    return m->window(w);
}

UiState::Window UiState::exclusive()
{
    if (mode() != Mode::Game)
    {
        return Window::count;
    }
    for (W w : kExclusive)
    {
        if (visible(window(w)))
        {
            return w;
        }
    }
    return Window::count;
}

cUI_Window2* UiState::exclusiveWindow()
{
    const W w = exclusive();
    return w == Window::count ? nullptr : window(w);
}

bool UiState::shown(Window w)
{
    if (w == Window::escapeMenu)
    {
        // Drawn over everything, in cinematic mode too.
        const cUI_Manager* m = manager();
        return m && (m->flags & cUI_Manager::inGame) && !(m->flags & cUI_Manager::inMenus) && visible(window(w));
    }
    if (mode() != Mode::Game || !visible(window(w)))
    {
        return false;
    }
    const W alone = exclusive();
    return alone == Window::count || alone == w;
}

bool UiState::cursorWindowShown()
{
    for (int i = 0; i < static_cast<int>(Window::count); ++i)
    {
        const auto w = static_cast<W>(i);
        if (!isHud(w) && shown(w))
        {
            return true;
        }
    }
    return false;
}

cUI_Window2* UiState::menu(int index)
{
    cUI_Manager* m = manager();
    if (!m || mode() != Mode::Menus || index < 0 || index >= cUI_Manager::menuCount ||
        !(m->flags & cUI_Manager::menusCreated))
    {
        return nullptr;
    }
    cUI_Window2* screen = m->menus[index];
    return visible(screen) ? screen : nullptr;
}

cUI_BusyDlg* UiState::messageBox()
{
    cUI_Manager* m = manager();
    cUI_BusyDlg* box = m ? m->dialog : nullptr;
    return visible(box) ? box : nullptr;
}

cUI_EscMenu* UiState::escapeMenu()
{
    return shown(Window::escapeMenu) ? static_cast<cUI_EscMenu*>(window(Window::escapeMenu)) : nullptr;
}

cUI_Control2* UiState::focus()
{
    const cUI_Manager* m = manager();
    cUI_Control2* control = m ? m->focus : nullptr;
    return control && (control->flags & cUI_Control2::focusFlag) ? control : nullptr;
}

bool UiState::typing()
{
    return focus() != nullptr || shown(Window::console);
}

bool UiState::helpScreen()
{
    const cUI_Manager* m = manager();
    return m && (m->flags & cUI_Manager::helpScreen);
}

bool UiState::inputBlocked()
{
    const cUI_Manager* m = manager();
    return m && (m->flags & cUI_Manager::inputBlocked);
}

const char* UiState::name(Window w)
{
    return w < Window::count ? kNames[static_cast<int>(w)] : "?";
}
