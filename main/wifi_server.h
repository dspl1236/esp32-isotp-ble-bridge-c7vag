#ifndef WIFI_SERVER_H
#define WIFI_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi operating modes */
typedef enum {
    WIFI_MODE_DISABLED = 0,
    WIFI_MODE_AP       = 1,   /* Access Point + captive portal */
    WIFI_MODE_STATION  = 2,   /* Join existing network */
} funkbridge_wifi_mode_t;

/* WiFi config stored in NVS */
typedef struct {
    funkbridge_wifi_mode_t mode;
    char ssid[64];
    char password[64];
} wifi_config_nvs_t;

/* Start WiFi subsystem — call after NVS init */
void wifi_server_start(void);
void wifi_server_stop(void);

/* Read current mode from NVS */
funkbridge_wifi_mode_t wifi_get_mode(void);

/* Called by ISO-TP bridge to push frames to WebSocket clients */
void wifi_server_push_frame(uint16_t tx_id, uint16_t rx_id,
                             const uint8_t *data, size_t len);

/* Called by WebSocket receive handler to inject frames into ISO-TP */
typedef void (*wifi_frame_cb_t)(uint16_t tx_id, uint16_t rx_id,
                                 const uint8_t *data, size_t len);
void wifi_server_set_rx_callback(wifi_frame_cb_t cb);

#ifdef __cplusplus
}
#endif
#endif /* WIFI_SERVER_H */
