#ifndef MCP2515_H
#define MCP2515_H

#include "freertos/FreeRTOS.h"
#include "driver/twai.h"
#include "constants.h"

/* SPI Commands */
#define MCP2515_CMD_RESET        0xC0
#define MCP2515_CMD_READ         0x03
#define MCP2515_CMD_WRITE        0x02
#define MCP2515_CMD_BIT_MODIFY   0x05
#define MCP2515_CMD_RTS_TX0      0x81
#define MCP2515_CMD_READ_RX0     0x90
#define MCP2515_CMD_READ_RX1     0x94

/* Registers */
#define MCP2515_REG_CANSTAT      0x0E
#define MCP2515_REG_CANCTRL      0x0F
#define MCP2515_REG_CNF3         0x28
#define MCP2515_REG_CNF2         0x29
#define MCP2515_REG_CNF1         0x2A
#define MCP2515_REG_CANINTE      0x2B
#define MCP2515_REG_CANINTF      0x2C
#define MCP2515_REG_TXB0CTRL     0x30
#define MCP2515_REG_TXB0SIDH     0x31
#define MCP2515_REG_TXB0DLC      0x35
#define MCP2515_REG_TXB0D0       0x36
#define MCP2515_REG_RXB0CTRL     0x60
#define MCP2515_REG_RXB1CTRL     0x70
#define MCP2515_REG_RXM0SIDH     0x20
#define MCP2515_REG_RXM1SIDH     0x24

/* CANCTRL modes */
#define MCP2515_MODE_NORMAL      0x00
#define MCP2515_MODE_CONFIG      0x80
#define MCP2515_MODE_MASK        0xE0

/* Interrupt flags */
#define MCP2515_INT_RX0IF        0x01
#define MCP2515_INT_RX1IF        0x02

/* Bit timing: 8 MHz crystal -> 100 kbps
 * TQ = 2*(BRP+1)/Fosc = 2*4/8M = 1us
 * NTQ = SYNC(1)+PRSEG(3)+PHSEG1(3)+PHSEG2(3) = 10 -> 100 kbps */
#define MCP2515_CNF1_100K        0x03
#define MCP2515_CNF2_100K        0x92
#define MCP2515_CNF3_100K        0x02

void mcp2515_init(void);
void mcp2515_deinit(void);
void mcp2515_start_task(void);
void mcp2515_stop_task(void);
void mcp2515_send(twai_message_t *msg);

#endif
