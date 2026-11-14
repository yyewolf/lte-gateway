// Package config loads the server configuration from a YAML file, with
// LTEGTW_* environment variables taking precedence.
package config

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"

	"gopkg.in/yaml.v3"
)

type Config struct {
	Gateway struct {
		// URL of the gateway API over the tailnet, e.g. http://100.64.125.24
		URL   string `yaml:"url"`
		Token string `yaml:"token"`
	} `yaml:"gateway"`

	RTP struct {
		// Listen address for call audio from the gateway, e.g. ":4000"
		Listen string `yaml:"listen"`
	} `yaml:"rtp"`

	Discord struct {
		Token          string   `yaml:"token"`
		GuildID        string   `yaml:"guild_id"`
		ChannelID      string   `yaml:"channel_id"`
		VoiceChannelID string   `yaml:"voice_channel_id"`
		AllowedUserIDs []string `yaml:"allowed_user_ids"`
		// Users mentioned when a call comes in
		NotifyUserIDs []string `yaml:"notify_user_ids"`
	} `yaml:"discord"`

	// Directory for WAV recordings of calls; empty disables recording.
	RecordingsDir string `yaml:"recordings_dir"`
	LogLevel      string `yaml:"log_level"`
}

// Load reads path (optional, may be empty) then applies environment overrides.
func Load(path string) (*Config, error) {
	c := &Config{}
	c.RTP.Listen = ":4000"
	c.LogLevel = "info"

	if path != "" {
		b, err := os.ReadFile(path)
		if err != nil && !(errors.Is(err, os.ErrNotExist) && path == DefaultPath) {
			return nil, err
		}
		if err == nil {
			if err := yaml.Unmarshal(b, c); err != nil {
				return nil, fmt.Errorf("%s: %w", path, err)
			}
		}
	}

	env := map[string]*string{
		"LTEGTW_GATEWAY_URL":              &c.Gateway.URL,
		"LTEGTW_GATEWAY_TOKEN":            &c.Gateway.Token,
		"LTEGTW_RTP_LISTEN":               &c.RTP.Listen,
		"LTEGTW_DISCORD_TOKEN":            &c.Discord.Token,
		"LTEGTW_DISCORD_GUILD_ID":         &c.Discord.GuildID,
		"LTEGTW_DISCORD_CHANNEL_ID":       &c.Discord.ChannelID,
		"LTEGTW_DISCORD_VOICE_CHANNEL_ID": &c.Discord.VoiceChannelID,
		"LTEGTW_RECORDINGS_DIR":           &c.RecordingsDir,
		"LTEGTW_LOG_LEVEL":                &c.LogLevel,
	}
	for k, p := range env {
		if v, ok := os.LookupEnv(k); ok {
			*p = v
		}
	}
	if v, ok := os.LookupEnv("LTEGTW_DISCORD_ALLOWED_USER_IDS"); ok {
		c.Discord.AllowedUserIDs = splitList(v)
	}
	if v, ok := os.LookupEnv("LTEGTW_DISCORD_NOTIFY_USER_IDS"); ok {
		c.Discord.NotifyUserIDs = splitList(v)
	}
	return c, c.validate()
}

const DefaultPath = "config.yaml"

func splitList(v string) []string {
	var out []string
	for _, s := range strings.Split(v, ",") {
		if s = strings.TrimSpace(s); s != "" {
			out = append(out, s)
		}
	}
	return out
}

func (c *Config) validate() error {
	var missing []string
	if c.Gateway.URL == "" {
		missing = append(missing, "gateway.url")
	}
	if c.Discord.Token == "" {
		missing = append(missing, "discord.token")
	}
	if c.Discord.GuildID == "" {
		missing = append(missing, "discord.guild_id")
	}
	if c.Discord.ChannelID == "" {
		missing = append(missing, "discord.channel_id")
	}
	if len(missing) > 0 {
		return fmt.Errorf("missing config: %s", strings.Join(missing, ", "))
	}
	for _, id := range append(append([]string{c.Discord.GuildID, c.Discord.ChannelID}, c.Discord.AllowedUserIDs...), c.Discord.NotifyUserIDs...) {
		if _, err := strconv.ParseUint(id, 10, 64); err != nil {
			return fmt.Errorf("invalid discord id %q", id)
		}
	}
	if c.Discord.VoiceChannelID != "" {
		if _, err := strconv.ParseUint(c.Discord.VoiceChannelID, 10, 64); err != nil {
			return fmt.Errorf("invalid discord voice_channel_id %q", c.Discord.VoiceChannelID)
		}
	}
	return nil
}
