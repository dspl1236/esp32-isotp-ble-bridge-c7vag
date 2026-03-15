#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "isotp.h"
#include "isotp_link_containers.h"
#include "constants.h"

#define LINKS_TAG   "ISOTP_LINKS"

// Raw sniff toggle — controlled via BLE settings command BRG_SETTING_RAW_SNIFF
uint8_t raw_sniff_enabled = 0;

// ─── Helper ──────────────────────────────────────────────────────────────────
static void init_container(IsoTpLinkContainer *c, uint16_t number,
                            const char *name, uint32_t tx_id, uint32_t rx_id,
                            uint32_t buf_size)
{
    c->number      = number;
    c->buffer_size = buf_size;
    strncpy(c->name, name, sizeof(c->name) - 1);

    c->recv_buf    = calloc(1, buf_size);
    c->send_buf    = calloc(1, buf_size);
    c->payload_buf = calloc(1, buf_size);
    assert(c->recv_buf && c->send_buf && c->payload_buf);

    // isotp_init_link(link, send_id=TX, recv_id=RX, ...)
    isotp_init_link(&c->link, tx_id, rx_id,
                    c->send_buf, buf_size,
                    c->recv_buf,  buf_size);
}

void configure_isotp_links()
{
    disable_isotp_links();

#ifdef PROFILE_C7_VAG
    // ── C7 Audi A6/A7/A8 (4G/4H, Lear J533) ─────────────────────────────────
    // Slot  Module   TX(→ECU)  RX(←ECU)  Notes
    //  0    J533     0x710     0x77A     Gateway / CP master
    //  1    J255     0x746     0x7B0     Climatronic
    //  2    J136     0x74C     0x7B6     Driver seat memory
    //  3    J521     0x74D     0x7B7     Passenger seat memory
    //  4    ECU      0x7E0     0x7E8     3.0T/3.2T engine (Simos12.x/PCR2.1)
    //  5    TCU      0x7E1     0x7E9     ZF 8HP45/8HP70
    //  6    J104     0x760     0x768     ABS/ESC (Bosch 9.0)
    //  7    spare    0x700     0x7E8     user-defined / broadcast
    init_container(&isotp_link_containers[0], 0, "j533_gateway",   0x710, 0x77A, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[1], 1, "j255_hvac",      0x746, 0x7B0, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[2], 2, "j136_seat_drv",  0x74C, 0x7B6, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[3], 3, "j521_seat_pass", 0x74D, 0x7B7, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[4], 4, "ecu_engine",     0x7E0, 0x7E8, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[5], 5, "tcu_zf8hp",      0x7E1, 0x7E9, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[6], 6, "j104_abs",       0x760, 0x768, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[7], 7, "spare_dtc",      0x700, 0x7E8, ISOTP_BUFFER_SIZE_SMALL);
    ESP_LOGI(LINKS_TAG, "Profile: C7_VAG (8 containers)");
#else
    // ── MQB tuning profile (original BridgeLEG) ──────────────────────────────
    init_container(&isotp_link_containers[0], 0, "isotp_container_ecu",    0x7E0, 0x7E8, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[1], 1, "isotp_container_tcu",    0x7E1, 0x7E9, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[2], 2, "isotp_container_haldex", 0x7E5, 0x7ED, ISOTP_BUFFER_SIZE);
    init_container(&isotp_link_containers[3], 3, "isotp_container_dtc",    0x700, 0x7E8, ISOTP_BUFFER_SIZE_SMALL);
    ESP_LOGI(LINKS_TAG, "Profile: MQB (4 containers)");
#endif

    for (uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++) {
        IsoTpLinkContainer *c = &isotp_link_containers[i];
        c->wait_for_isotp_data_sem = xSemaphoreCreateBinary();
        c->task_mutex              = xSemaphoreCreateMutex();
        c->data_mutex              = xSemaphoreCreateMutex();
    }
    isotp_link_container_id = 0;
    ESP_LOGI(LINKS_TAG, "Init");
}

void disable_isotp_links()
{
    bool16 didDeinit = false;
    for (uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++) {
        IsoTpLinkContainer *c = &isotp_link_containers[i];
        if (c->wait_for_isotp_data_sem) { vSemaphoreDelete(c->wait_for_isotp_data_sem); c->wait_for_isotp_data_sem = NULL; didDeinit = true; }
        if (c->task_mutex)              { vSemaphoreDelete(c->task_mutex);              c->task_mutex = NULL;              didDeinit = true; }
        if (c->data_mutex)              { vSemaphoreDelete(c->data_mutex);              c->data_mutex = NULL;              didDeinit = true; }
        if (c->recv_buf)    { free(c->recv_buf);    c->recv_buf    = NULL; didDeinit = true; }
        if (c->send_buf)    { free(c->send_buf);    c->send_buf    = NULL; didDeinit = true; }
        if (c->payload_buf) { free(c->payload_buf); c->payload_buf = NULL; didDeinit = true; }
    }
    if (didDeinit) ESP_LOGI(LINKS_TAG, "Deinit");
}
