# GameServer.exe stand-in for tests

Copy of the game folder only. The game starts `GameServer.exe` when you create a LAN game; it passes one number
(not the original's command line) and the remake server (SacredEngineRemake, `SacredGameServer.exe`) refuses
unknown options. This launcher takes that place: it starts `SacredGameServer.exe` from the same folder with
`--accept-any-data --game-dir=<folder> --public-address=10.9.19.20 --udp-port=0 --network-lobby=` (edit
`Program.cs` for your machine's LAN address) and waits for it.

    dotnet build -c Release tools/gs-launcher
    # in the COPY of the game folder: rename GameServer.exe to GameServer.orig.exe, copy the remake server's build
    # output (SacredGameServer.*) and this launcher's GameServer.* next to Sacred.exe

Kill `SacredGameServer.exe` of the copy after a run (the game only kills the launcher).
