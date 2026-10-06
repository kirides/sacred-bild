#pragma once
#include <string>

// Text and speech in the game's language (settings.cfg LANGUAGE, or the code in lower case on the command line).
// The GOG builds ignore it for both: their text is built into the exe and the speech is always PAK\sound.pak.
// SacredBild loads scripts\<code>\global.res and PAK\sound.<code>.pak instead when they are there.
namespace Language
{
    // Hooks the text and sound file loading; call inside a Patch transaction.
    void install();

    // The game's text for a key ("UI_CFG_AUTOSAVE"), as the game looks it up (0x672740): the id is a hash of the key
    // in upper case. Empty before the text is loaded or for an unknown key.
    std::wstring text(const char* key);
}
