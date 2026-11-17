using Lua;
using Lua.Standard;
// args: harness.lua mod_main.lua scenario.lua
var state = LuaState.Create();
state.OpenBasicLibrary(); state.OpenStringLibrary(); state.OpenTableLibrary(); state.OpenMathLibrary();
state.Environment["print"] = new LuaFunction("print", (c, _) => { var parts = new List<string>(); for (var i = 0; i < c.ArgumentCount; i++) parts.Add(c.GetArgument(i).ToString()); Console.WriteLine(string.Join("\t", parts)); return new ValueTask<int>(c.Return()); });
try
{
    foreach (var file in args) { await state.DoStringAsync(File.ReadAllText(file), Path.GetFileName(file)); }
    Console.WriteLine("OK");
}
catch (Exception e) { Console.WriteLine("FAIL: " + e.Message); Environment.Exit(1); }
