// Pinned tailnet peers: kept in NVS, handed to microlink as priority peers. A pinned peer
// always gets a tunnel slot; when the table (CONFIG_ML_MAX_PEERS) is full, the
// least-recently-used unpinned peer is evicted for it.
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#define PEERS_MAX_PINNED 8          // = MICROLINK_MAX_PINNED

typedef struct microlink_s microlink_t;

void peers_init(void);                   // loads the list from NVS
void peers_apply(microlink_t *ml);       // hands it to microlink (call after microlink_init)

// Pins / unpins a tailnet IP (host byte order) and saves the list. The hostname is looked up
// among the known peers. why gets a reason on error.
esp_err_t peers_pin(uint32_t ip, const char **why);
esp_err_t peers_unpin(uint32_t ip, const char **why);

// {"peers": [...], "dropped": [...], "pinned": [...], "max_peers": n, "max_pinned": n}
cJSON *peers_json(void);
void peers_print(void);                  // console
