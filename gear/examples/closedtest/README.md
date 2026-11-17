# closedtest: an automation run into a Closed Net game on this machine

Drives Multiplayer > Closed Internet with a test lobby (sacred-lobby, `lobby.closed_net: true`, listening on loopback), picks or
creates a hero, joins the game server listed in the Closed Net room and starts the world. Used by the end-to-end tests of the shared
stash and the auction house (`-gear-mods=closedtest,shared-stash` with `selftest = 'e2e'`).

Setup (everything loopback, test ports, a COPY of the game folder, never the player's folder):

1. Lobby: build sacred-lobby (`go build ./cmd/sacred-lobby`), a folder with `lobby.yaml` (`listen: 127.0.0.1:17066`, `lobby.closed_net: true`,
   channel 1 with flag `closednet`, `hero_templates: templates`) and the game's `Templates\Hero00..07.ptx` copied to `data\templates`.
   `sacred-lobby -config lobby.yaml adduser NAME PASSWORD` makes an account (no Register dialog needed).
2. Game copy: copy the game folder; rename its `GameServer.exe` to `GameServer.orig.exe`; put the stand-in launcher (`tools/gs-launcher`) and the
   remake server build (`SacredGameServer.*`) there; `launcher.args` (one argument per line, `{dir}` is the folder) holds the server options:
   `--game-dir={dir} --bind=127.0.0.1 --network-port-tcp=26806 --network-port-listen=26805 --udp-port=26905 --lan-announce=0 --webui-port=18806
   --network-lobby=127.0.0.1 --network-lobbyport=17066 --lobby-channel=1 --closed-net=1 --authoritative --auth-strict-items=log
   --scripts-dir={dir}\scripts --scripts-data={dir}\script-data --server-name=ClosedTest` (+ `--trace=1` to log every message).
   Start it with `GameServer.exe 0` before the game: it registers in the lobby and shows up in the Closed Net room.
3. Run: `Sacred.exe -gear-auto -gear-instance=NAME -gear-mods=closedtest` in the copy, `config.lua` of the mod: `return {create = true}` once for a
   new account (makes the hero), then without it. The settings window must be hidden (`HideSettingsWindow=1` in the copy's SacredBild.ini).

What the run found (details in the commit history): the account fields come filled with `guest`; the host pane's Starten does nothing until
the game password (NETWORK_PASSWORD of the server's Gameserver.cfg) is typed into the pane's password field.
