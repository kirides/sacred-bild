using System.Diagnostics;
// Test launcher: stands in for GameServer.exe in a copy of the game folder and starts the remake server (SacredGameServer.exe).
var dir = AppContext.BaseDirectory;
File.AppendAllText(Path.Combine(dir, "launcher.log"), $"{DateTime.Now:HH:mm:ss} args: [{string.Join("] [", args)}] cwd {Environment.CurrentDirectory}\n");
var psi = new ProcessStartInfo(Path.Combine(dir, "SacredGameServer.exe")) { WorkingDirectory = dir, UseShellExecute = false };
var argsFile = Path.Combine(dir, "launcher.args");
if (File.Exists(argsFile))
{
    // one argument per line (tests that need other options, e.g. Closed Net against a test lobby); {dir} is this folder
    foreach (var line in File.ReadAllLines(argsFile))
        if (line.Trim().Length > 0) psi.ArgumentList.Add(line.Trim().Replace("{dir}", dir.TrimEnd('\\')));
}
else
{
    psi.ArgumentList.Add("--accept-any-data"); psi.ArgumentList.Add("--game-dir=" + dir.TrimEnd('\\')); psi.ArgumentList.Add("--public-address=10.9.19.20");
    psi.ArgumentList.Add("--udp-port=0"); psi.ArgumentList.Add("--network-lobby=");
}
var child = Process.Start(psi)!;
AppDomain.CurrentDomain.ProcessExit += (_, _) => { try { if (!child.HasExited) child.Kill(true); } catch { } };
child.WaitForExit();
return child.ExitCode;
