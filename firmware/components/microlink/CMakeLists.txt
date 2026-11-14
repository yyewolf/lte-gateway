set(SRCS
    "src/microlink.c"
    "src/ml_coord.c"
    "src/ml_derp.c"
    "src/ml_net_io.c"
    "src/ml_wg_mgr.c"
    "src/ml_stun.c"
    "src/ml_noise.c"
    "src/ml_h2.c"
    "src/ml_udp.c"
    "src/ml_tcp.c"
    "src/ml_peer_nvs.c"
    "src/nacl_box.c"
    "src/x25519.c"
    "src/ml_zerocopy.c"
    "src/ml_cellular.c"
    "src/ml_at_socket.c"
    "src/ml_net_switch.c"
    "src/ml_config_httpd.c"
)

if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(ML_JSON espressif__cjson)  # cJSON is a managed component since ESP-IDF 6.0
else()
    set(ML_JSON json)
endif()

set(REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    lwip
    mbedtls
    ${ML_JSON}
    esp_driver_uart
    esp_driver_gpio
    esp_driver_tsens
    esp_http_server
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES
        wireguard_lwip
)

# GCC 15 (ESP-IDF 6.x) rejects fixed-size byte arrays initialised from strings without a NUL
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unterminated-string-initialization)
