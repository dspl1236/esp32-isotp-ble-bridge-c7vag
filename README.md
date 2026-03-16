# esp32-isotp-ble-bridge — C7 VAG fork

Fork of [Switchleg1/esp32-isotp-ble-bridge](https://github.com/Switchleg1/esp32-isotp-ble-bridge)
/ [bri3d/esp32-isotp-ble-bridge](https://github.com/bri3d/esp32-isotp-ble-bridge).

Adds a **C7 VAG build profile** for the Audi A6/A7/A8 (4G/4H platform) alongside the
original MQB Simos18/DQ250 tuning profile. Also adds a **raw CAN sniff mode** useful
for protocol research. No other changes to the tool's intent — this is still a BLE↔ISO-TP
bridge and J2534 device, nothing more.

Higher-level tooling (UDS client, CAL parser, J533 probe, companion app) lives in
[simos-suite](https://github.com/dspl1236/simos-suite).

---

## What changed vs upstream

### 1. `PROFILE_C7_VAG` build flag

Replaces the four hardcoded MQB containers with eight C7-correct ones:

| Slot | Module | TX→ECU | RX←ECU | Notes |
|------|--------|--------|--------|-------|
| 0 | ECU (Simos8.5) | 0x7E0 | 0x7E8 | 3.0T TFSI — primary target for VW_Flash `--simos8` |
| 1 | TCU (ZF 8HP) | 0x7E1 | 0x7E9 | 8-speed automatic |
| 2 | J533 Gateway | 0x710 | 0x77A | CAN gateway (CP research — see simos-suite) |
| 3 | J255 HVAC | 0x746 | 0x7B0 | Climatronic |
| 4 | J136 Seat Drv | 0x74C | 0x7B6 | Driver seat memory |
| 5 | J521 Seat Pass | 0x74D | 0x7B7 | Passenger seat memory |
| 6 | J104 ABS/ESC | 0x760 | 0x768 | Bosch 9.0 |
| 7 | Broadcast/DTC | 0x700 | 0x7E8 | spare / user-defined |

The MQB profile (Simos18/DQ250/Haldex) is unchanged and remains the default.

### 2. Raw CAN sniff mode (`BRG_SETTING_RAW_SNIFF = 9`)

Send BLE settings command with ID 9, payload `0x01` to enable. When active:
- Every received CAN frame is forwarded to BLE **before** ISO-TP filtering
- The `< 0x500` ID filter is bypassed
- BLE packets arrive with `txID = rxID = 0xCAFE` as a sentinel
- Frame format: `[id_hi][id_lo][dlc][d0..d7]`
- ISO-TP processing continues normally in parallel

Disable with payload `0x00`.

---

## Building

**Prerequisites:** [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/)

```bash
# MQB profile — original Simos18/DQ250 (default, unchanged)
idf.py build
idf.py -p /dev/ttyUSB0 flash

# C7 VAG profile — Audi A6/A7/A8 C7 platform
idf.py build -DPROFILE=C7_VAG
idf.py -p /dev/ttyUSB0 flash
```

---

## Hardware

Works on Macchina A0 or the [AMAleg DIY clone](https://github.com/Switchleg1/AMAleg).

| GPIO | Signal | OBD-II |
|------|--------|--------|
| 5 | CAN TX | Pin 6 |
| 4 | CAN RX | Pin 14 |
| 21 | Silent (LOW) | — |

---

## Supported software

Same as upstream — this fork is a drop-in replacement:

- **[VW_Flash](https://github.com/bri3d/VW_Flash)** — use `--simos8` for the C7 3.0T
- **[SimosTools](https://play.google.com/store/apps/details?id=com.app.simostools)** — Android
- Any J2534 software

---

## Credits

Original firmware by [bri3d](https://github.com/bri3d) and [Switchleg1](https://github.com/Switchleg1).
