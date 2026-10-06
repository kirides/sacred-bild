// Matchmaker for SacredBild: hosts publish their Sacred games here, players list them in Sacred's LAN list and get
// introduced to the host (UDP hole punching). Protocol: docs/UDP_PROTOCOL.md.
package main

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type Config struct {
	Listen             string
	WebListen          string
	LogSensitiveData   bool
	GameTimeout        time.Duration
	RefreshMs          uint32
	MaxGames           int
	MaxGamesPerAddress int
	RequestsPerSecond  float64
	Title              string
}

func defaultConfig() Config {
	return Config{
		Listen:             ":2107",
		WebListen:          ":8080",
		GameTimeout:        20 * time.Second,
		RefreshMs:          5000,
		MaxGames:           1000,
		MaxGamesPerAddress: 4,
		RequestsPerSecond:  20,
		Title:              "Sacred games",
	}
}

// configKeys are the ini keys; each is also a flag in lower camel case (-listen, -webListen, ...).
var configKeys = []string{"Listen", "WebListen", "LogSensitiveData", "GameTimeout", "RefreshMs", "MaxGames",
	"MaxGamesPerAddress", "RequestsPerSecond", "Title"}

func flagName(key string) string { return strings.ToLower(key[:1]) + key[1:] }

func (c *Config) set(key, value string) error {
	value = strings.TrimSpace(value)
	positive := func() (int, error) {
		n, err := strconv.Atoi(value)
		if err != nil || n < 1 {
			return 0, fmt.Errorf("%s: want a number >= 1, got %q", key, value)
		}
		return n, nil
	}
	switch strings.ToLower(key) {
	case "listen":
		c.Listen = value
	case "weblisten":
		c.WebListen = value
	case "logsensitivedata":
		switch strings.ToLower(value) {
		case "1", "true", "yes", "on":
			c.LogSensitiveData = true
		case "0", "false", "no", "off", "":
			c.LogSensitiveData = false
		default:
			return fmt.Errorf("%s: want 0 or 1, got %q", key, value)
		}
	case "gametimeout":
		n, err := positive()
		c.GameTimeout = time.Duration(n) * time.Second
		return err
	case "refreshms":
		n, err := positive()
		c.RefreshMs = uint32(n)
		return err
	case "maxgames":
		n, err := positive()
		c.MaxGames = n
		return err
	case "maxgamesperaddress":
		n, err := positive()
		c.MaxGamesPerAddress = n
		return err
	case "requestspersecond":
		f, err := strconv.ParseFloat(value, 64)
		if err != nil || f <= 0 {
			return fmt.Errorf("%s: want a number > 0, got %q", key, value)
		}
		c.RequestsPerSecond = f
	case "title":
		c.Title = value
	default:
		return fmt.Errorf("unknown key %q", key)
	}
	return nil
}

// loadIni reads Key=Value lines; ';' and '#' start comments, [Section] lines are ignored.
func loadIni(path string, c *Config) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	scanner := bufio.NewScanner(f)
	for line := 1; scanner.Scan(); line++ {
		text := strings.TrimSpace(scanner.Text())
		if text == "" || text[0] == ';' || text[0] == '#' || text[0] == '[' {
			continue
		}
		key, value, ok := strings.Cut(text, "=")
		if !ok {
			return fmt.Errorf("%s:%d: expected Key=Value", path, line)
		}
		if err := c.set(strings.TrimSpace(key), value); err != nil {
			return fmt.Errorf("%s:%d: %w", path, line, err)
		}
	}
	return scanner.Err()
}

func loadConfig(args []string) (Config, error) {
	cfg := defaultConfig()
	fs := flag.NewFlagSet("matchmaker", flag.ContinueOnError)
	configPath := fs.String("config", "", "config file (default: matchmaker.ini next to the executable, if there is one)")
	values := map[string]*string{}
	for _, key := range configKeys {
		values[key] = fs.String(flagName(key), "", "overrides "+key+" from the config file")
	}
	if err := fs.Parse(args); err != nil {
		return cfg, err
	}

	path := *configPath
	if path == "" {
		if exe, err := os.Executable(); err == nil {
			path = filepath.Join(filepath.Dir(exe), "matchmaker.ini")
			if _, err := os.Stat(path); err != nil {
				path = ""
			}
		}
	}
	if path != "" {
		if err := loadIni(path, &cfg); err != nil {
			return cfg, err
		}
		log.Printf("config: %s", path)
	}

	var err error
	fs.Visit(func(f *flag.Flag) {
		for _, key := range configKeys {
			if f.Name == flagName(key) && err == nil {
				err = cfg.set(key, *values[key])
			}
		}
	})
	if err == nil && time.Duration(cfg.RefreshMs)*time.Millisecond >= cfg.GameTimeout {
		err = fmt.Errorf("RefreshMs (%d) must be shorter than GameTimeout (%v)", cfg.RefreshMs, cfg.GameTimeout)
	}
	return cfg, err
}

func run() error {
	cfg, err := loadConfig(os.Args[1:])
	if err != nil {
		return err
	}
	log.Printf("Listen=%s WebListen=%s LogSensitiveData=%v GameTimeout=%v RefreshMs=%d MaxGames=%d "+
		"MaxGamesPerAddress=%d RequestsPerSecond=%g Title=%q", cfg.Listen, cfg.WebListen, cfg.LogSensitiveData,
		cfg.GameTimeout, cfg.RefreshMs, cfg.MaxGames, cfg.MaxGamesPerAddress, cfg.RequestsPerSecond, cfg.Title)

	conns, err := listenUDP(cfg.Listen)
	if err != nil {
		return err
	}
	closeAll := func() {
		for _, c := range conns {
			c.Close()
		}
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	server := NewServer(cfg, conns...)

	var web *http.Server
	if cfg.WebListen != "" {
		web = newWebServer(cfg.WebListen, newWebHandler(server, cfg.Title))
		listener, err := net.Listen("tcp", cfg.WebListen)
		if err != nil {
			closeAll()
			return fmt.Errorf("WebListen: %w", err)
		}
		log.Printf("web UI on http://%s/", listener.Addr())
		go func() {
			if err := web.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
				log.Printf("web: %v", err)
			}
		}()
	}

	go func() {
		<-ctx.Done()
		log.Printf("shutting down")
		closeAll() // ends Serve's read loops
	}()
	err = server.Serve(ctx)
	if web != nil {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = web.Shutdown(shutdownCtx)
	}
	return err
}

// listenUDP opens the matchmaker's sockets. Without a host (":2107") there is one per family, IPv4 and IPv6 (the
// IPv6 one only where the system has IPv6); separate sockets don't depend on dual-stack sockets, which some
// systems turn off. With an address, just that one.
func listenUDP(listen string) ([]*net.UDPConn, error) {
	host, port, err := net.SplitHostPort(listen)
	if err != nil {
		return nil, fmt.Errorf("Listen: %w", err)
	}
	if host != "" {
		addr, err := net.ResolveUDPAddr("udp", listen)
		if err != nil {
			return nil, fmt.Errorf("Listen: %w", err)
		}
		network := "udp6"
		if addr.IP.To4() != nil {
			network = "udp4"
		}
		conn, err := net.ListenUDP(network, addr)
		if err != nil {
			return nil, err
		}
		log.Printf("matchmaker on UDP %s", conn.LocalAddr())
		return []*net.UDPConn{conn}, nil
	}
	portNumber, err := strconv.Atoi(port)
	if err != nil || portNumber < 0 || portNumber > 0xFFFF {
		return nil, fmt.Errorf("Listen: bad port %q", port)
	}
	conn4, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero, Port: portNumber})
	if err != nil {
		return nil, err
	}
	conns := []*net.UDPConn{conn4}
	log.Printf("matchmaker on UDP %s (IPv4)", conn4.LocalAddr())
	if conn6, err := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6unspecified, Port: portNumber}); err != nil {
		log.Printf("no IPv6: %v", err)
	} else {
		conns = append(conns, conn6)
		log.Printf("matchmaker on UDP %s (IPv6)", conn6.LocalAddr())
	}
	return conns, nil
}

func main() {
	if err := run(); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			os.Exit(0)
		}
		log.Fatal(err)
	}
}
