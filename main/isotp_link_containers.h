#ifndef __ISOTP_LINK_CONTAINERS_H__
#define __ISOTP_LINK_CONTAINERS_H__

// ─── Profile selection ───────────────────────────────────────────────────────
// Compile with -DPROFILE_C7_VAG to target Audi C7 A6/A7/A8 modules.
// Default (no flag) keeps the original MQB Simos18/DQ250 tuning profile.
//
// C7_VAG profile container layout:
//   0  J533  Gateway            TX 0x710  RX 0x77A
//   1  J255  Climatronic HVAC   TX 0x746  RX 0x7B0
//   2  J136  Seat Driver Mem    TX 0x74C  RX 0x7B6
//   3  J521  Seat Pass  Mem     TX 0x74D  RX 0x7B7
//   4  ECU   Engine (3.0T/3.2T) TX 0x7E0  RX 0x7E8  (Simos12.x/PCR2.1)
//   5  TCU   ZF 8HP DSG         TX 0x7E1  RX 0x7E9
//   6  J104  ABS/ESC            TX 0x760  RX 0x768
//   7  Spare / user-defined     TX 0x700  RX 0x7E8
//
// MQB profile (default) container layout:
//   0  ECU    Simos18            TX 0x7E0  RX 0x7E8
//   1  TCU    DQ250/DQ381        TX 0x7E1  RX 0x7E9
//   2  Haldex Gen5               TX 0x7E5  RX 0x7ED
//   3  DTC    Broadcast          TX 0x700  RX 0x7E8

#ifdef PROFILE_C7_VAG
  #define NUM_ISOTP_LINK_CONTAINERS 8
#else
  #define NUM_ISOTP_LINK_CONTAINERS 4
#endif

// ─── Raw CAN sniff mode ──────────────────────────────────────────────────────
// When raw_sniff_enabled != 0, twai_receive_task forwards every CAN frame
// over BLE/UART before ISO-TP filtering.  The BLE packet uses:
//   txID = 0xCAFE  rxID = 0xCAFE  (magic sentinel the host uses to identify raw frames)
// Frame payload format: [id_hi][id_lo][dlc][d0..d7]  (3 + up-to-8 bytes)
extern uint8_t raw_sniff_enabled;

// ─── Struct ──────────────────────────────────────────────────────────────────
typedef struct IsoTpLinkContainer {
    char    name[32];
    IsoTpLink link;
    SemaphoreHandle_t wait_for_isotp_data_sem;
    SemaphoreHandle_t data_mutex;
    SemaphoreHandle_t task_mutex;
    uint8_t *recv_buf;
    uint8_t *send_buf;
    uint8_t *payload_buf;
    uint32_t buffer_size;
    uint16_t number;
    bool16   use_conv_can;
} IsoTpLinkContainer;

IsoTpLinkContainer isotp_link_containers[NUM_ISOTP_LINK_CONTAINERS];
uint16_t isotp_link_container_id;

void configure_isotp_links();
void disable_isotp_links();

#endif
