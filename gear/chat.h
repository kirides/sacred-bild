#pragma once
#include <cstdint>
#include <string>

// The chat and message lines of the game, for mods (gear.chat).
//
// Every line the game shows in its message list at the top of the screen (chat, the server's messages, the game's own
// notes such as a trade being declined, the player's own lines) goes through one function: cUI_TextLines::add
// (ENG 007237D0, thiscall (lines, channel, font, const wstring* text, ARGB color), ret 0x10), on the UI manager's list
// at +0x14C (through ENG 0075A770) or on the list of a window that has one (0075A7A0, the trade window 0075C790). The
// channel picks the default colour: 1 white, 2 light blue (private), 3 tomato (the server), 4 green (the player's own
// lines); 0x10 is a separate list of notices. Gear detours that function, copies the line and, while a mod asks for it,
// does not add the line to the UI manager's list (the game's message lines stay empty: the mod shows them itself). The
// other lists and the notices are always left alone.
//
// Who sent a chat line: the receive dispatcher's chat case (types 3 private, 4 team, 5 everyone; gear/netmsg.cpp) hands
// each message to Chat::noteNet before the game formats and adds it (ENG 007ED4A0, on the same thread, at once); the
// line that ends with that message's text is paired with its type and sender slot (the name as the game shows it, the
// players' object's names at +0x384C + slot * 12, else the hero's name).
namespace Chat
{
    // Inside the Patch transaction. False if the function was not found (no lines then).
    bool install();
    bool available();

    // The receive dispatcher's chat case, for each chat message that is not a Gear frame: `type` 3 / 4 / 5, the sender's
    // slot (0: the server), its text.
    void noteNet(uint16_t type, uint32_t slot, const uint16_t* text, size_t units);

    enum class Kind : uint8_t
    {
        System,     // the game's own lines (unpaired, channel 1)
        Global,     // chat to everyone (type 5)
        Team,       // chat to the team (type 4)
        Private,    // a private message to this player (type 3)
        Server,     // a message of the game server (slot 0, or channel 3)
        Own,        // the player's own line (channel 4)
        Notice,     // channel 0x10
    };

    struct Line
    {
        std::wstring text;          // as the game got it, with its backslash codes
        std::wstring sender;        // paired chat only: the name the game shows
        std::wstring message;       // paired chat only: what was written
        uint32_t color = 0;         // ARGB, the channel's default unless the caller gave one
        uint32_t slot = 0;          // paired chat only: the sender's slot (0: the server)
        uint16_t channel = 0;
        Kind kind = Kind::System;
        bool main = true;           // the UI manager's list (false: a window's own list)
        bool hidden = false;        // Gear kept it out of the game's list
    };

    // The oldest line not taken yet. Engine thread.
    bool take(Line& out);

    // Called every Lua tick: whether some mod shows the lines itself. The game's list stays empty while this was true
    // within the last 0.5 s, so the lines come back by themselves when the mod stops (unloaded, errors, a loading screen).
    void setHidden(bool hidden);
    bool hidden();

    // Self tests (gear.auto.chat_add): adds a line to the UI manager's message list as the game does (through the
    // hook). Engine thread, in game. False if there is no list.
    bool addForTest(const std::wstring& text, uint16_t channel, uint32_t color);
}
