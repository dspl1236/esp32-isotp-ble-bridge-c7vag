#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "soc/dport_reg.h"
#include "isotp.h"
#include "ble_server.h"
#include "isotp_link_containers.h"
#include "twai.h"
#include "persist.h"
#include "constants.h"
#include "led.h"
#include "eeprom.h"
#include "uart.h"
#include "connection_handler.h"
#include "isotp_bridge.h"
#include "mcp2515.h"

#define MAIN_TAG    "Main"

// Storage for the global semaphore declared (extern) in constants.h
SemaphoreHandle_t sync_task_sem;

void app_main(void)
{
    ESP_LOGI(MAIN_TAG, "Application starting");

#if !CONFIG_ESP_TASK_WDT_INIT
    ESP_ERROR_CHECK(esp_task_wdt_init(WDT_TIMEOUT_S, true));
    ESP_LOGI(MAIN_TAG, "WDT initialized");
#endif

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_ERROR_CHECK(esp_task_wdt_status(NULL));

    sync_task_sem = xSemaphoreCreateBinary();

    eeprom_init();
    ble_server_init();
    ch_init();
    led_init();
    uart_init();
    twai_init();
    mcp2515_init();
    isotp_init();
    persist_init();

    char* gapName = eeprom_read_str(BLE_GAP_KEY);
    if (gapName) {
        ble_set_gap_name(gapName, false);
        free(gapName);
    }

#if SLEEP_MODE == 1
    while(1) {
#endif
        /* BLE transport */
        ble_server_callbacks callbacks = {
            .data_received             = bridge_received_ble,
            .notifications_subscribed  = bridge_connect,
            .notifications_unsubscribed = bridge_disconnect
        };
        ble_server_start(callbacks);

        /* Start CAN/ISO-TP tasks */
        twai_start_task();
        mcp2515_start_task();
        isotp_start_task();
        persist_start_task();
        uart_start_task();
        ch_start_task();

        /* Wait for sleep command */
        while (ch_take_sleep_sem() != pdTRUE)
            esp_task_wdt_reset();

        /* Stop transport */
        ble_stop_advertising();
        while (ble_connected()) {
            vTaskDelay(pdMS_TO_TICKS(TIMEOUT_NORMAL));
            esp_task_wdt_reset();
        }
        ble_server_stop();

        ch_stop_task();
        uart_stop_task();
        persist_stop_task();
        isotp_stop_task();
        mcp2515_stop_task();
        twai_stop_task();

        esp_sleep_enable_timer_wakeup(SLEEP_TIME * US_TO_S);
        ESP_LOGI(MAIN_TAG, "Timeout - Sleeping [%ds]", SLEEP_TIME);

#if SLEEP_MODE == 1
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(TIMEOUT_LONG));
        esp_task_wdt_reset();
        esp_light_sleep_start();
        esp_task_wdt_reset();
    }
#endif

    persist_deinit();
    isotp_deinit();
    mcp2515_deinit();
    twai_deinit();
    uart_deinit();
    led_deinit();
    ch_deinit();
    ble_server_deinit();

    ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    esp_deep_sleep_start();
}
