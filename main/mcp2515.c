/*
 * mcp2515.c - MCP2515 SPI CAN controller (Convenience CAN, 100kbps)
 * 8 MHz crystal on module board.
 * Pins: SCK=18 MOSI=23 MISO=19 CS=17 INT=34 (input-only, for interrupt polling)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "mcp2515.h"
#include "isotp.h"
#include "isotp_link_containers.h"
#include "constants.h"

#define MCP_TAG "MCP2515"

static spi_device_handle_t  s_spi        = NULL;
static SemaphoreHandle_t    s_mutex      = NULL;
static SemaphoreHandle_t    s_task_mutex = NULL;
static bool16               s_run        = false;

/* ---- low-level SPI ---------------------------------------------------- */

static void mcp_write(uint8_t reg, uint8_t val) {
    uint8_t tx[3] = {MCP2515_CMD_WRITE, reg, val};
    spi_transaction_t t = {.length=24, .tx_buffer=tx};
    spi_device_transmit(s_spi, &t);
}

static uint8_t mcp_read(uint8_t reg) {
    uint8_t tx[3]={MCP2515_CMD_READ,reg,0}, rx[3]={0};
    spi_transaction_t t = {.length=24, .tx_buffer=tx, .rx_buffer=rx};
    spi_device_transmit(s_spi, &t);
    return rx[2];
}

static void mcp_bit_mod(uint8_t reg, uint8_t mask, uint8_t data) {
    uint8_t tx[4] = {MCP2515_CMD_BIT_MODIFY, reg, mask, data};
    spi_transaction_t t = {.length=32, .tx_buffer=tx};
    spi_device_transmit(s_spi, &t);
}

static void mcp_reset(void) {
    uint8_t cmd = MCP2515_CMD_RESET;
    spi_transaction_t t = {.length=8, .tx_buffer=&cmd};
    spi_device_transmit(s_spi, &t);
    vTaskDelay(pdMS_TO_TICKS(15));
}

static bool16 mcp_set_mode(uint8_t mode) {
    mcp_bit_mod(MCP2515_REG_CANCTRL, MCP2515_MODE_MASK, mode);
    for (int i=0; i<20; i++) {
        if ((mcp_read(MCP2515_REG_CANSTAT) & MCP2515_MODE_MASK) == mode) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

/* ---- init / deinit ----------------------------------------------------- */

void mcp2515_init(void) {
    mcp2515_deinit();

    spi_bus_config_t bus = {
        .mosi_io_num=MCP2515_MOSI_PIN, .miso_io_num=MCP2515_MISO_PIN,
        .sclk_io_num=MCP2515_SCK_PIN,  .quadwp_io_num=-1, .quadhd_io_num=-1,
        .max_transfer_sz=64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz=MCP2515_CLOCK_HZ, .mode=0,
        .spics_io_num=MCP2515_CS_PIN, .queue_size=8,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_spi));

    s_mutex      = xSemaphoreCreateMutex();
    s_task_mutex = xSemaphoreCreateMutex();

    mcp_reset();

    if (!mcp_set_mode(MCP2515_MODE_CONFIG)) {
        ESP_LOGE(MCP_TAG, "Config mode failed"); return;
    }

    /* 100 kbps, 8 MHz crystal */
    mcp_write(MCP2515_REG_CNF1, MCP2515_CNF1_100K);
    mcp_write(MCP2515_REG_CNF2, MCP2515_CNF2_100K);
    mcp_write(MCP2515_REG_CNF3, MCP2515_CNF3_100K);

    /* Accept all frames (zero masks) */
    for (uint8_t r=MCP2515_REG_RXM0SIDH; r<=MCP2515_REG_RXM0SIDH+3; r++) mcp_write(r,0);
    for (uint8_t r=MCP2515_REG_RXM1SIDH; r<=MCP2515_REG_RXM1SIDH+3; r++) mcp_write(r,0);
    mcp_write(MCP2515_REG_RXB0CTRL, 0x64); /* RXM=11(any), BUKT=1 */
    mcp_write(MCP2515_REG_RXB1CTRL, 0x60); /* RXM=11(any) */

    mcp_write(MCP2515_REG_CANINTE, MCP2515_INT_RX0IF | MCP2515_INT_RX1IF);
    mcp_write(MCP2515_REG_CANINTF, 0x00);

    gpio_config_t ic = {.pin_bit_mask=(1ULL<<MCP2515_INT_PIN),
                        .mode=GPIO_MODE_INPUT, .pull_up_en=GPIO_PULLUP_ENABLE,
                        .intr_type=GPIO_INTR_DISABLE};
    gpio_config(&ic);

    if (!mcp_set_mode(MCP2515_MODE_NORMAL)) {
        ESP_LOGE(MCP_TAG, "Normal mode failed"); return;
    }
    ESP_LOGI(MCP_TAG, "Ready — 100kbps Convenience CAN");
}

void mcp2515_deinit(void) {
    if (s_spi) { spi_bus_remove_device(s_spi); spi_bus_free(SPI3_HOST); s_spi=NULL; }
    if (s_mutex)      { vSemaphoreDelete(s_mutex);      s_mutex=NULL; }
    if (s_task_mutex) { vSemaphoreDelete(s_task_mutex); s_task_mutex=NULL; }
}

/* ---- send -------------------------------------------------------------- */

void mcp2515_send(twai_message_t *msg) {
    if (!s_spi || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t sidh = (msg->identifier >> 3) & 0xFF;
    uint8_t sidl = (msg->identifier & 0x07) << 5;
    uint8_t tx[15] = {MCP2515_CMD_WRITE, MCP2515_REG_TXB0SIDH,
                      sidh, sidl, 0, 0,
                      msg->data_length_code & 0x0F};
    memcpy(tx+7, msg->data, msg->data_length_code);
    spi_transaction_t t = {.length=(7+msg->data_length_code)*8, .tx_buffer=tx};
    spi_device_transmit(s_spi, &t);
    uint8_t rts = MCP2515_CMD_RTS_TX0;
    spi_transaction_t t2 = {.length=8, .tx_buffer=&rts};
    spi_device_transmit(s_spi, &t2);
    xSemaphoreGive(s_mutex);
}

/* ---- rx task ----------------------------------------------------------- */

extern void mcp2515_deliver_frame(twai_message_t *msg);

static void mcp_read_rxbuf(uint8_t cmd, twai_message_t *out) {
    uint8_t tx[14]={cmd}, rx[14]={0};
    spi_transaction_t t = {.length=14*8, .tx_buffer=tx, .rx_buffer=rx};
    spi_device_transmit(s_spi, &t);
    out->identifier        = ((uint32_t)rx[1]<<3) | (rx[2]>>5);
    out->data_length_code  = rx[5] & 0x0F;
    memcpy(out->data, rx+6, out->data_length_code);
}

static void mcp2515_rx_task(void *arg) {
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    xSemaphoreTake(s_task_mutex, portMAX_DELAY);
    ESP_LOGI(MCP_TAG, "RX task started");
    xSemaphoreGive(sync_task_sem);

    while (s_run) {
        if (gpio_get_level(MCP2515_INT_PIN) == 0) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            uint8_t intf = mcp_read(MCP2515_REG_CANINTF);
            twai_message_t frame = {0};

            if (intf & MCP2515_INT_RX0IF) {
                mcp_read_rxbuf(MCP2515_CMD_READ_RX0, &frame);
                mcp_bit_mod(MCP2515_REG_CANINTF, MCP2515_INT_RX0IF, 0x00);
                xSemaphoreGive(s_mutex);
                if (frame.identifier >= 0x500) mcp2515_deliver_frame(&frame);
            } else if (intf & MCP2515_INT_RX1IF) {
                mcp_read_rxbuf(MCP2515_CMD_READ_RX1, &frame);
                mcp_bit_mod(MCP2515_REG_CANINTF, MCP2515_INT_RX1IF, 0x00);
                xSemaphoreGive(s_mutex);
                if (frame.identifier >= 0x500) mcp2515_deliver_frame(&frame);
            } else {
                mcp_bit_mod(MCP2515_REG_CANINTF, 0xFF, 0x00);
                xSemaphoreGive(s_mutex);
            }
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(MCP_TAG, "RX task stopped");
    xSemaphoreGive(s_task_mutex);
    ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
}

void mcp2515_start_task(void) {
    mcp2515_stop_task();
    s_run = true;
    xSemaphoreTake(sync_task_sem, 0);
    xTaskCreate(mcp2515_rx_task, "MCP_rx", TASK_STACK_SIZE, NULL, TWAI_TASK_PRIO, NULL);
    xSemaphoreTake(sync_task_sem, portMAX_DELAY);
}

void mcp2515_stop_task(void) {
    if (s_run) {
        s_run = false;
        xSemaphoreTake(s_task_mutex, portMAX_DELAY);
        xSemaphoreGive(s_task_mutex);
    }
}
