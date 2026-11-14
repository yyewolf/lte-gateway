// Persistent settings in NVS (namespace "gw"), set from the USB console.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    const char *key;
    const char *help;
    bool secret;      // masked by 'config'
} setting_def_t;

extern const setting_def_t g_settings[];
extern const size_t g_settings_count;

esp_err_t settings_init(void);
// Copies the value (or "" when unset) into out. Returns true when set and non-empty.
bool settings_get(const char *key, char *out, size_t len);
esp_err_t settings_set(const char *key, const char *value);   // NULL or "" erases
const setting_def_t *settings_find(const char *key);
