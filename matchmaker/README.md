# SacredBild matchmaker

A small server for playing Sacred over the internet with SacredBild. Hosts publish the games they host here, and
players see them in Sacred's own LAN list. When a player joins, the matchmaker introduces player and host to each
other so that SacredBild's UDP transport gets through home routers without port forwarding (UDP hole punching).
Game traffic never passes through the matchmaker. Protocol: [docs/UDP_PROTOCOL.md](../docs/UDP_PROTOCOL.md).

A web page (`WebListen`) lists the current games: name, players, game version, UDP transport, and how long each
has been running. `/api/games` returns the same list as JSON, and `/healthz` returns `ok`. Neither shows addresses.

## Build

Go 1.27, standard library only:

```sh
go build                       # matchmaker / matchmaker.exe for this system
go test ./...
```

Cross-compiling (static binaries, no cgo):

```sh
CGO_ENABLED=0 GOOS=linux   GOARCH=amd64 go build -o matchmaker-linux-amd64
CGO_ENABLED=0 GOOS=linux   GOARCH=arm64 go build -o matchmaker-linux-arm64
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -o matchmaker-windows-amd64.exe
CGO_ENABLED=0 GOOS=darwin  GOARCH=arm64 go build -o matchmaker-darwin-arm64
```

PowerShell: `$env:CGO_ENABLED=0; $env:GOOS="linux"; $env:GOARCH="amd64"; go build -o matchmaker-linux-amd64`.

## Configuration

`matchmaker.ini` next to the executable (or `-config <path>`), `Key=Value` lines; see the sample in this folder.
Every key can be overridden on the command line in lower camel case: `-listen=:2107 -webListen= -gameTimeout=30`.

| Key | Default | Meaning |
|---|---|---|
| Listen | `:2107` | UDP address for hosts and players (IPv4). |
| WebListen | `:8080` | HTTP address of the web page; empty = none. |
| LogSensitiveData | 0 | 1 = log IP addresses and ports. |
| GameTimeout | 20 | Seconds without a REGISTER after which a game is dropped. |
| RefreshMs | 5000 | How often hosts send REGISTER (must be shorter than GameTimeout). |
| MaxGames | 1000 | Games listed at most. |
| MaxGamesPerAddress | 4 | Games per host IP address. |
| RequestsPerSecond | 20 | Requests per IP address and second (bursts of twice that); more are dropped. |
| Title | Sacred games | Heading of the web page. |

Open the UDP port (`Listen`) in the firewall, and the TCP port of `WebListen` if the page should be reachable.
Players and hosts set `[Net] Matchmaker=<server>:2107` in `SacredBild.ini`.

## Privacy

The matchmaker has to know the addresses of hosts and players to introduce them, and players receive the
address of the host they list or join (Sacred connects to it). It keeps them in memory only for as long as a game
is listed. It doesn't show them on the web page or in `/api/games`, and doesn't log them unless
`LogSensitiveData=1`. It also removes the LAN address Sacred puts into its announcements before passing them on.

## Running as a service (systemd)

```ini
# /etc/systemd/system/sacred-matchmaker.service
[Unit]
Description=SacredBild matchmaker
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/opt/sacred-matchmaker/matchmaker -config /opt/sacred-matchmaker/matchmaker.ini
DynamicUser=yes
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

`systemctl enable --now sacred-matchmaker`; the log goes to the journal (`journalctl -u sacred-matchmaker`).
Ports below 1024 would need `AmbientCapabilities=CAP_NET_BIND_SERVICE`; the defaults don't.
