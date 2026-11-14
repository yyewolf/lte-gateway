if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(GW_JSON espressif__cjson)
else()
    set(GW_JSON json)
endif()

idf_component_register(SRCS "main.c" "settings.c" "board.c" "modem.c" "net.c" "api.c" "console.c" "setup.c" "pdu.c" "sms_store.c" "sms.c"
                       INCLUDE_DIRS "."
                       REQUIRES microlink console nvs_flash esp_wifi esp_netif esp_event esp_http_server ${GW_JSON}
                                esp_driver_gpio esp_driver_uart esp_adc esp_timer lwip app_update)
