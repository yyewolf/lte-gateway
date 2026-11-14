// Command ltegtw-server bridges the LTE gateway to Discord: SMS, calls, and
// call audio in a voice channel.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"github.com/yyewolf/ltegtw/server/internal/config"
	"github.com/yyewolf/ltegtw/server/internal/discordbot"
	"github.com/yyewolf/ltegtw/server/internal/gateway"
	"github.com/yyewolf/ltegtw/server/internal/rtp"
)

// The gateway sends and receives call audio on this UDP port.
const gatewayRTPPort = 4000

func main() {
	cfgPath := flag.String("config", config.DefaultPath, "path to the YAML config (LTEGTW_* environment variables override it)")
	flag.Parse()

	if err := run(*cfgPath); err != nil {
		fmt.Fprintln(os.Stderr, "ltegtw-server:", err)
		os.Exit(1)
	}
}

func run(cfgPath string) error {
	cfg, err := config.Load(cfgPath)
	if err != nil {
		return err
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: parseLevel(cfg.LogLevel)}))
	slog.SetDefault(log)

	gw, err := gateway.New(cfg.Gateway.URL, cfg.Gateway.Token)
	if err != nil {
		return err
	}

	// Only accept call audio from the gateway itself.
	ips, err := net.LookupIP(gw.Host())
	if err != nil {
		return fmt.Errorf("resolve gateway %s: %w", gw.Host(), err)
	}
	allow := func(ip net.IP) bool {
		for _, a := range ips {
			if a.Equal(ip) {
				return true
			}
		}
		return false
	}
	relay, err := rtp.Listen(cfg.RTP.Listen, &net.UDPAddr{IP: ips[0], Port: gatewayRTPPort}, allow, log.With("component", "rtp"))
	if err != nil {
		return fmt.Errorf("rtp listen %s: %w", cfg.RTP.Listen, err)
	}
	defer relay.Close()

	b, err := discordbot.New(cfg, gw, relay, log)
	if err != nil {
		return err
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	log.Info("starting", slog.String("gateway", cfg.Gateway.URL), slog.Int("rtp_port", relay.LocalPort()))
	return b.Run(ctx)
}

func parseLevel(s string) slog.Level {
	switch strings.ToLower(s) {
	case "debug":
		return slog.LevelDebug
	case "warn", "warning":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	}
	return slog.LevelInfo
}
