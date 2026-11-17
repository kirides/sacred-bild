using System.Diagnostics;
// Test launcher: stands in for GameServer.exe in a copy of the game folder and starts the remake server (SacredGameServer.exe).
var dir = AppContext.BaseDirectory;
File.AppendAllText(Path.Combine(dir, "launcher.log"), $"{DateTime.Now:HH:mm:ss} args: [{string.Join("] [", args)}] cwd {Environment.CurrentDirectory}\n");
var psi = new ProcessStartInfo(Path.Combine(dir, "SacredGameServer.exe")) { WorkingDirectory = dir, UseShellExecute = false };
psi.ArgumentList.Add("--accept-any-data"); psi.ArgumentList.Add("--game-dir=" + dir.TrimEnd('\\')); psi.ArgumentList.Add("--public-address=10.9.19.20");
psi.ArgumentList.Add("--udp-port=0"); psi.ArgumentList.Add("--network-lobby=");
var child = Process.Start(psi)!;
AppDomain.CurrentDomain.ProcessExit += (_, _) => { try { if (!child.HasExited) child.Kill(true); } catch { } };
child.WaitForExit();
return child.ExitCode;
