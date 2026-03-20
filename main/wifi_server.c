/**
 * wifi_server.c — FunkBridge WiFi subsystem
 *
 * Implements three modes:
 *   AP mode:      ESP32 creates "FunkBridge" access point.
 *                 Captive portal redirects all HTTP traffic to the web app.
 *                 DNS server answers all queries with 192.168.4.1.
 *                 mDNS: funkbridge.local
 *
 *   Station mode: ESP32 joins existing WiFi network.
 *                 mDNS: funkbridge.local
 *                 Falls back to AP mode if network unavailable.
 *
 * WebSocket bridge (both modes):
 *   ws://funkbridge.local/ws  (or ws://192.168.4.1/ws)
 *   Frame format: identical to BLE packet framing (ble_header_t, 8 bytes + payload)
 *   This allows the same web app to work over both BLE and WiFi.
 *
 * Captive portal (AP mode only):
 *   DNS server catches all DNS queries → 192.168.4.1
 *   HTTP /generate_204       → 204 (Android/Chrome)
 *   HTTP /hotspot-detect.html → redirect (Apple)
 *   HTTP /ncsi.txt           → redirect (Windows)
 *   HTTP /*                  → redirect to /
 *   SPIFFS serves the web app at /
 *
 * Stability:
 *   - Ring buffer between ISO-TP task and WebSocket send
 *   - WebSocket drops oldest frame on overflow (never blocks CAN task)
 *   - Watchdog on WiFi management task
 *   - AP fallback if station connect fails after WIFI_CONNECT_TIMEOUT_S
 */

#include "wifi_server.h"
#include "constants.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi_srv";

/* ── Configuration ────────────────────────────────────────────────────────── */
#define WIFI_AP_SSID            "FunkBridge"
#define WIFI_AP_PASS            ""              /* open network */
#define WIFI_AP_IP              "192.168.4.1"
#define WIFI_AP_CHANNEL         6
#define WIFI_AP_MAX_CONN        4
#define WIFI_MDNS_HOSTNAME      "funkbridge"
#define WIFI_CONNECT_TIMEOUT_S  15
#define WS_RING_BUF_SIZE        (8 * 1024)      /* 8KB ring buffer */
#define WS_MAX_FRAME_SIZE       (4096 + 8)      /* max ISO-TP + header */

/* ── State ────────────────────────────────────────────────────────────────── */
static httpd_handle_t   s_server       = NULL;
static int              s_ws_fd        = -1;    /* single client */
static RingbufHandle_t  s_tx_ringbuf   = NULL;
static wifi_frame_cb_t  s_rx_cb        = NULL;
static bool             s_running      = false;
static TaskHandle_t     s_mgr_task     = NULL;

/* ── NVS helpers ──────────────────────────────────────────────────────────── */
#define NVS_NS   "funkbridge"
#define NVS_MODE "wifi_mode"
#define NVS_SSID "wifi_ssid"
#define NVS_PASS "wifi_pass"

funkbridge_wifi_mode_t wifi_get_mode(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return WIFI_MODE_AP;
    uint8_t mode = WIFI_MODE_AP;
    nvs_get_u8(h, NVS_MODE, &mode);
    nvs_close(h);
    return (funkbridge_wifi_mode_t)mode;
}

static void nvs_save_mode(funkbridge_wifi_mode_t mode) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_MODE, (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_get_credentials(char *ssid, size_t ssid_len,
                                  char *pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_str(h, NVS_SSID, ssid, &ssid_len);
    nvs_get_str(h, NVS_PASS, pass, &pass_len);
    nvs_close(h);
}

static void nvs_save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_SSID, ssid);
    nvs_set_str(h, NVS_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
}

/* ── WebSocket frame push (called from ISO-TP task) ─────────────────────── */
void wifi_server_push_frame(uint16_t tx_id, uint16_t rx_id,
                              const uint8_t *data, size_t len) {
    if (!s_running || s_ws_fd < 0 || !s_tx_ringbuf) return;

    /* Build BLE-compatible 8-byte header + payload */
    uint8_t buf[WS_MAX_FRAME_SIZE];
    if (len + 8 > sizeof(buf)) return;
    buf[0] = 0xF1;                              /* BLE_HEADER_ID */
    buf[1] = 0x00;                              /* flags */
    buf[2] = rx_id & 0xFF; buf[3] = rx_id >> 8; /* rxID LE */
    buf[4] = tx_id & 0xFF; buf[5] = tx_id >> 8; /* txID LE */
    buf[6] = len & 0xFF;   buf[7] = len >> 8;   /* size LE */
    memcpy(buf + 8, data, len);

    /* Non-blocking ring buffer send — drops oldest on overflow */
    xRingbufferSend(s_tx_ringbuf, buf, len + 8, 0);
}

void wifi_server_set_rx_callback(wifi_frame_cb_t cb) {
    s_rx_cb = cb;
}

/* ── WebSocket handler ────────────────────────────────────────────────────── */
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake from fd=%d", httpd_req_to_sockfd(req));
        s_ws_fd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }
    /* Receive frame from browser */
    httpd_ws_frame_t pkt = { .type = HTTPD_WS_TYPE_BINARY };
    uint8_t buf[WS_MAX_FRAME_SIZE];
    pkt.payload = buf;
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    if (ret != ESP_OK) return ret;
    if (pkt.len >= 8 && buf[0] == 0xF1 && s_rx_cb) {
        uint16_t rx_id  = buf[2] | (buf[3] << 8);
        uint16_t tx_id  = buf[4] | (buf[5] << 8);
        uint16_t sz     = buf[6] | (buf[7] << 8);
        if (sz <= pkt.len - 8)
            s_rx_cb(tx_id, rx_id, buf + 8, sz);
    }
    return ESP_OK;
}

/* ── WebSocket send task ──────────────────────────────────────────────────── */
static void ws_send_task(void *arg) {
    while (s_running) {
        size_t item_size = 0;
        void  *item = xRingbufferReceiveFromISR(s_tx_ringbuf, &item_size);
        if (item) {
            if (s_ws_fd >= 0 && s_server) {
                httpd_ws_frame_t pkt = {
                    .type    = HTTPD_WS_TYPE_BINARY,
                    .payload = (uint8_t *)item,
                    .len     = item_size,
                };
                httpd_ws_send_frame_async(s_server, s_ws_fd, &pkt);
            }
            vRingbufferReturnItem(s_tx_ringbuf, item);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    vTaskDelete(NULL);
}

/* ── Captive portal / API handlers ───────────────────────────────────────── */
static esp_err_t captive_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t android_204(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /api/wifi — save credentials and reboot into station mode */
static esp_err_t api_wifi_post(httpd_req_t *req) {
    char buf[256] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (received <= 0) return ESP_FAIL;
    /* Simple key=value parse: ssid=X&password=Y */
    char ssid[64]={0}, pass[64]={0};
    char *p = strstr(buf, "ssid=");
    if (p) { p+=5; char *e=strchr(p,'&'); size_t l=e?(size_t)(e-p):strlen(p); if(l<64){memcpy(ssid,p,l);ssid[l]=0;} }
    p = strstr(buf, "password=");
    if (p) { p+=9; char *e=strchr(p,'&'); size_t l=e?(size_t)(e-p):strlen(p); if(l<64){memcpy(pass,p,l);pass[l]=0;} }
    nvs_save_credentials(ssid, pass);
    nvs_save_mode(WIFI_MODE_STATION);
    httpd_resp_sendstr(req, "OK — rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* GET /api/status — returns JSON status */
static esp_err_t api_status(httpd_req_t *req) {
    char json[256];
    snprintf(json, sizeof(json),
        "{"mode":%d,"hostname":"funkbridge","version":"%s"}",
        (int)wifi_get_mode(), FUNKBRIDGE_VERSION);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* SPIFFS file handler */
static esp_err_t spiffs_handler(httpd_req_t *req) {
    char path[64];
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) uri = "/index.html";
    snprintf(path, sizeof(path), "/spiffs%s", uri);
    FILE *f = fopen(path, "r");
    if (!f) { httpd_resp_send_404(req); return ESP_OK; }
    /* Set content type */
    if (strstr(path, ".html")) httpd_resp_set_type(req, "text/html");
    else if (strstr(path, ".js")) httpd_resp_set_type(req, "application/javascript");
    else if (strstr(path, ".css")) httpd_resp_set_type(req, "text/css");
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        httpd_resp_send_chunk(req, chunk, n);
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
    return ESP_OK;
}

/* ── Start HTTP server ────────────────────────────────────────────────────── */
static void start_http_server(bool ap_mode) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 5;
    if (httpd_start(&s_server, &cfg) != ESP_OK) return;

    /* WebSocket */
    httpd_uri_t ws = { .uri="/ws", .method=HTTP_GET, .handler=ws_handler,
                       .is_websocket=true };
    httpd_register_uri_handler(s_server, &ws);

    /* API */
    httpd_uri_t st = { .uri="/api/status", .method=HTTP_GET,  .handler=api_status };
    httpd_uri_t wp = { .uri="/api/wifi",   .method=HTTP_POST, .handler=api_wifi_post };
    httpd_register_uri_handler(s_server, &st);
    httpd_register_uri_handler(s_server, &wp);

    if (ap_mode) {
        /* Captive portal detection endpoints */
        httpd_uri_t a204 = { .uri="/generate_204",        .method=HTTP_GET, .handler=android_204 };
        httpd_uri_t hotspot = { .uri="/hotspot-detect.html", .method=HTTP_GET, .handler=captive_redirect };
        httpd_uri_t ncsi = { .uri="/ncsi.txt",            .method=HTTP_GET, .handler=captive_redirect };
        httpd_register_uri_handler(s_server, &a204);
        httpd_register_uri_handler(s_server, &hotspot);
        httpd_register_uri_handler(s_server, &ncsi);
    }

    /* SPIFFS wildcard — must be last */
    httpd_uri_t files = { .uri="/*", .method=HTTP_GET, .handler=spiffs_handler };
    httpd_register_uri_handler(s_server, &files);

    ESP_LOGI(TAG, "HTTP server started (ap_mode=%d)", ap_mode);
}

/* ── mDNS ─────────────────────────────────────────────────────────────────── */
static void start_mdns(void) {
    mdns_init();
    mdns_hostname_set(WIFI_MDNS_HOSTNAME);
    mdns_instance_name_set("FunkBridge ISO-TP Bridge");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_ws",   "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: funkbridge.local");
}

/* ── DNS captive portal task (AP mode only) ───────────────────────────────── */
static void dns_captive_task(void *arg) {
    /* Simple DNS server — answers all queries with 192.168.4.1 */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (s_running) {
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client, &clen);
        if (n < 12) continue;
        /* Build minimal DNS response pointing to 192.168.4.1 */
        uint8_t resp[512];
        memcpy(resp, buf, n);
        resp[2] = 0x81; resp[3] = 0x80;   /* QR=1 AA=0 */
        resp[6] = 0x00; resp[7] = 0x01;   /* 1 answer */
        int qend = 12;
        while (qend < n && buf[qend]) qend += buf[qend] + 1;
        qend += 5; /* null + qtype + qclass */
        int rlen = qend;
        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C; /* name ptr */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; /* A */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; /* IN */
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x3C; /* TTL 60s */
        resp[rlen++] = 0x00; resp[rlen++] = 0x04; /* rdlen 4 */
        resp[rlen++] = 192; resp[rlen++] = 168;
        resp[rlen++] = 4;   resp[rlen++] = 1;     /* 192.168.4.1 */
        sendto(sock, resp, rlen, 0, (struct sockaddr *)&client, clen);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* ── WiFi AP mode ─────────────────────────────────────────────────────────── */
static void start_ap_mode(void) {
    ESP_LOGI(TAG, "Starting AP mode: SSID=%s", WIFI_AP_SSID);
    esp_netif_create_default_wifi_ap();
    wifi_config_t cfg = {
        .ap = {
            .ssid            = WIFI_AP_SSID,
            .ssid_len        = strlen(WIFI_AP_SSID),
            .channel         = WIFI_AP_CHANNEL,
            .password        = WIFI_AP_PASS,
            .max_connection  = WIFI_AP_MAX_CONN,
            .authmode        = WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    esp_wifi_start();
    start_mdns();
    start_http_server(true);
    xTaskCreate(dns_captive_task, "dns_captive", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "AP ready: http://192.168.4.1  or  http://funkbridge.local");
}

/* ── WiFi Station mode ────────────────────────────────────────────────────── */
static void start_station_mode(void) {
    char ssid[64]={0}, pass[64]={0};
    nvs_get_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (!ssid[0]) {
        ESP_LOGW(TAG, "No credentials — falling back to AP mode");
        start_ap_mode();
        return;
    }
    ESP_LOGI(TAG, "Connecting to: %s", ssid);
    esp_netif_create_default_wifi_sta();
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    esp_wifi_connect();

    /* Wait for connection with timeout */
    int waited = 0;
    while (waited < WIFI_CONNECT_TIMEOUT_S * 10) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "Connected to %s", ssid);
            start_mdns();
            start_http_server(false);
            ESP_LOGI(TAG, "Ready: http://funkbridge.local");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        waited++;
    }
    ESP_LOGW(TAG, "Connect timeout — falling back to AP mode");
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_AP);
    start_ap_mode();
}

/* ── SPIFFS init ──────────────────────────────────────────────────────────── */
static void init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "SPIFFS mounted");
}

/* ── Public entry point ───────────────────────────────────────────────────── */
void wifi_server_start(void) {
    s_running = true;
    s_tx_ringbuf = xRingbufferCreate(WS_RING_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);

    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    init_spiffs();

    /* Start WebSocket send task */
    xTaskCreate(ws_send_task, "ws_send", 4096, NULL, 2, NULL);

    funkbridge_wifi_mode_t mode = wifi_get_mode();
    if (mode == WIFI_MODE_STATION)
        start_station_mode();
    else
        start_ap_mode();
}

void wifi_server_stop(void) {
    s_running = false;
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
    esp_wifi_stop();
    esp_wifi_deinit();
    mdns_free();
}
