Smoke tests for the `buildsharer` and `questhelper` example mods without the game: `harness.lua` is a strict mock of the parts of
the `gear` API those mods use (a call of a name the API docs do not list fails), the scenarios drive the panels and answer as the
server would. Needs the .NET SDK (the runner is LuaCSharp, the Lua of the SacredEngineRemake server).

    cd gear/tests/modmock
    dotnet build
    dotnet run --no-build -- harness.lua ../../examples/buildsharer/main.lua build_scenario.lua
    dotnet run --no-build -- harness.lua ../../examples/questhelper/main.lua quest_scenario.lua

Each prints `pass: ...` lines and `OK`, or `FAIL: ...` (exit code 1). This checks the mods' logic and their use of the documented API;
it does not draw anything and cannot tell how the panels look in the game.
