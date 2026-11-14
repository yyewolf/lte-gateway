package config

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoad(t *testing.T) {
	p := filepath.Join(t.TempDir(), "c.yaml")
	os.WriteFile(p, []byte(`
gateway:
  url: http://100.64.125.24
discord:
  token: file-token
  guild_id: "1"
  channel_id: "2"
  allowed_user_ids: ["3"]
`), 0o600)
	t.Setenv("LTEGTW_DISCORD_TOKEN", "env-token")
	t.Setenv("LTEGTW_DISCORD_NOTIFY_USER_IDS", "4, 5")
	c, err := Load(p)
	if err != nil {
		t.Fatal(err)
	}
	if c.Discord.Token != "env-token" || c.RTP.Listen != ":4000" || len(c.Discord.NotifyUserIDs) != 2 || c.Discord.AllowedUserIDs[0] != "3" {
		t.Fatalf("%+v", c)
	}
}

func TestValidate(t *testing.T) {
	t.Setenv("LTEGTW_GATEWAY_URL", "http://gw")
	t.Setenv("LTEGTW_DISCORD_TOKEN", "x")
	t.Setenv("LTEGTW_DISCORD_GUILD_ID", "1")
	if _, err := Load(""); err == nil {
		t.Fatal("missing channel_id accepted")
	}
	t.Setenv("LTEGTW_DISCORD_CHANNEL_ID", "abc")
	if _, err := Load(""); err == nil {
		t.Fatal("bad id accepted")
	}
	t.Setenv("LTEGTW_DISCORD_CHANNEL_ID", "2")
	if _, err := Load(""); err != nil {
		t.Fatal(err)
	}
	if _, err := Load("/nonexistent.yaml"); err == nil {
		t.Fatal("missing explicit file accepted")
	}
}
