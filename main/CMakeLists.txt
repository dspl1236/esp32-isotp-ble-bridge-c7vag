# Build configuration:
#   BLE firmware  — idf.py build
#   WiFi firmware — idf.py -DCONFIG_FUNKBRIDGE_WIFI_DEFAULT_MODE=1 build

if(CONFIG_FUNKBRIDGE_WIFI_DEFAULT_MODE)
    set(WIFI_SRCS "wifi_server.c")
    set(WIFI_REQUIRES
        esp_wifi
        esp_netif
        esp_http_server
        mdns
        spiffs
        lwip)
else()
    set(WIFI_SRCS "")
    set(WIFI_REQUIRES "")
endif()

idf_component_register(
    SRCS "main.c" "isotp_bridge.c" "isotp.c" "ble_server.c"
         "ws2812_control.c" "twai.c" "persist.c"
         "isotp_link_containers.c" "led.c" "eeprom.c"
         "uart.c" "connection_handler.c"
         ${WIFI_SRCS}
    INCLUDE_DIRS "."
    REQUIRES
        driver
        bt
        nvs_flash
        esp_timer
        ${WIFI_REQUIRES}
)
