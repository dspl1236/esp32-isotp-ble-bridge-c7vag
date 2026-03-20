idf_component_register(
    SRCS "main.c" "isotp_bridge.c" "isotp.c" "ble_server.c"
         "ws2812_control.c" "twai.c" "persist.c"
         "isotp_link_containers.c" "led.c" "eeprom.c"
         "uart.c" "connection_handler.c" "wifi_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_http_server
        mdns
        spiffs
        nvs_flash
        lwip
)
