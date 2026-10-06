# Languages

The GOG builds ignore the game's `LANGUAGE` for text and speech (the text is built into the exe, the speech is
always `PAK\sound.pak`). SacredBild loads the language's own files when they are there, so one install runs every
language you have the files for.

Sacred takes its language from `LANGUAGE` in `settings.cfg`, one of `US DE FR SP IT PL HU JP VC RU CZ` in upper
case (anything else quietly means `DE`), or from the code in lower case on the command line (`sacred.exe de`).
Both GOG installs ship with `LANGUAGE : US`, whatever their language. SacredBild uses the language's files if they
are there, and otherwise what the build ships with:

- text: `scripts\<code>\global.res`, else the text built into the exe;
- speech: `PAK\sound.<code>.pak`, else `PAK\sound.pak`.

`import-language.ps1` takes the text out of another install's exe, copies its `PAK\sound.pak` and stores both
under a code, e.g. German into the English install:

```powershell
.\import-language.ps1 -From "B:\Spiele\GOG Games\Sacred Gold" -Language DE -GameDir "B:\Spiele\GOG Games\sacred gold GOG"
```

Then set `LANGUAGE : DE`. `-HardLink` links `sound.pak` instead of copying it (400-470 MB, same drive only),
`-NoSpeech` takes only the text. A GOG install's `scripts\us\global.res` is a copy of its built-in text, so
`LANGUAGE : US` keeps the install's language until you import another one as `US`. To add English to the German
install, import it as `US`; `LANGUAGE : DE` then still gives German (no `scripts\de` and no `sound.de.pak`, so the
built-in text and `PAK\sound.pak`). `SacredBild.log` names the files in use (lines starting with `Language:`).

The code also changes a few things in the game: keyboard input for `PL`, IME and line breaking for `JP` and `VC`,
no status text on the loading screen for `SP`. Savegames store it; loading one saved under another code only logs
the difference.
