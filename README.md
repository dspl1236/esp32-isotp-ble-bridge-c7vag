# esp32-isotp-ble-bridge — C7 VAG Fork

Fork of [Switchleg1/esp32-isotp-ble-bridge](https://github.com/Switchleg1/esp32-isotp-ble-bridge) /
[bri3d/esp32-isotp-ble-bridge](https://github.com/bri3d/esp32-isotp-ble-bridge)

Adds a second build profile targeting the **Audi A6/A7/A8 C7 platform** (4G/4H, Lear J533 gateway)
alongside the original MQB Simos18 tuning profile. Also adds a **raw CAN sniff mode** for
protocol research and Component Protection investigation.

---

## New features vs upstream

### 1. C7 VAG build profile (`-DPROFILE=C7_VAG`)

Eight ISO-TP link containers pre-configured for C7 platform modules:

| Slot | Module | TX (→ECU) | RX (←ECU) | Purpose |
|------|--------|-----------|-----------|---------|
| 0 | J533 Gateway | 0x710 | 0x77A | CAN gateway / Component Protection master |
| 1 | J255 Climatronic | 0x746 | 0x7B0 | HVAC (CP research, 2-zone/4-zone) |
| 2 | J136 Seat Driver | 0x74C | 0x7B6 | Driver seat memory |
| 3 | J521 Seat Passenger | 0x74D | 0x7B7 | Passenger seat memory |
| 4 | ECU Engine | 0x7E0 | 0x7E8 | 3.0T/3.2T (Simos8.5 / S85 project) |
| 5 | TCU ZF 8HP | 0x7E1 | 0x7E9 | 8-speed automatic |
| 6 | J104 ABS/ESC | 0x760 | 0x768 | Bosch 9.0 ABS |
| 7 | Spare / DTC | 0x700 | 0x7E8 | User-defined or broadcast DTC |

Original MQB profile (4 containers: ECU/TCU/Haldex/DTC) unchanged when built without the flag.

### 2. Raw CAN sniff mode

Enable via BLE settings command `BRG_SETTING_RAW_SNIFF` (ID = 9), 1-byte payload (0=off, 1=on).

When active:
- Every received CAN frame is forwarded to BLE/UART **before** ISO-TP filtering
- The `< 0x500` ID filter is bypassed — all traffic is visible including low-ID broadcast frames
- BLE packets arrive with magic `txID = rxID = 0xCAFE` so the host can identify them
- Frame payload: `[id_hi][id_lo][dlc][d0..d7]` (3–11 bytes)
- ISO-TP processing continues normally in parallel

This mode is designed for Component Protection constellation research — sniff the full bus
during an ODIS session to capture the UDS sequences ODIS sends during CP operations.

---

## Building

### Prerequisites

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- ESP32 dev board (Macchina A0 or [AMAleg DIY clone](https://github.com/Switchleg1/AMAleg))

### MQB profile (original, default)
```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

### C7 VAG profile
```bash
idf.py build -DPROFILE=C7_VAG
idf.py -p /dev/ttyUSB0 flash
```

### Wiring (A0 / AMAleg clone)
| ESP32 GPIO | Signal | OBD-II pin |
|---|---|---|
| GPIO 5 | CAN TX | Pin 6 (CAN-H via transceiver) |
| GPIO 4 | CAN RX | Pin 14 (CAN-L via transceiver) |
| GPIO 21 | Silent (pulled LOW) | — |

---

## Supported software

- **[VW_Flash](https://github.com/bri3d/VW_Flash)** — ECU flashing. Use `--simos8` flag for C7 3.0T (Simos8.5 / S85 project code).
- **[SimosTools](https://play.google.com/store/apps/details?id=com.app.simostools)** — Android logging/flashing
- **C7 VAG Companion** (in `companion/`) — Python GUI for C7-specific operations: DID scanner, J533 constellation probe, persist poller, raw sniff viewer
- **python-udsoncan** — direct UDS scripting via J2534

---

## ECU notes — Audi A6 C7 3.0T TFSI (CGWA/CGWB)

The 3.0T uses **Continental Simos8.5**, project code `S85`. This is already fully supported
by VW_Flash. Block layout:

| Block | Name | Base address | Size |
|-------|------|-------------|------|
| 1 | BOOT (CBOOT) | 0x80020000 | 0x13E00 |
| 2 | SOFTWARE (ASW) | 0x80080000 | 0x17FE00 |
| 3 | CALIBRATION (CAL) | 0xA0040000 | 0x3C000 |

Crypto: **XOR** (not AES) — the simplest scheme in the Simos family.

SA2 seed/key script (hex):
```
6805824A10680493300419624A05871510197082499324041966824A058702031970824A0181494C
```

**CAL block is where fuel/ignition/boost tables live.** For lean diagnosis:
1. Read CAL block: `python VW_Flash.py --simos8 --interface J2534 --action read --outfile cal_backup.bin`
2. Inspect with TunerPro XDF or a hex editor at offset 0x40000 (CAL base in bin file)
3. Key tables for AFR: lambda setpoint map, injector scaling, MAF transfer function

---

## Component Protection research (C7 J533)

See [VAG-CP-Docs](https://github.com/dspl1236/VAG-CP-Docs) for full protocol documentation.

Quick workflow using this bridge in C7 VAG profile:

```python
# Probe J533 (slot 0) for constellation-related DIDs
import asyncio
from ble_bridge_client import BLEBridgeClient  # companion/ble_bridge_client.py

async def probe():
    async with BLEBridgeClient() as bridge:
        # Read J533 serial and VIN binding
        serial = await bridge.uds_read(tx=0x710, rx=0x77A, did=0xF18C)
        vin    = await bridge.uds_read(tx=0x710, rx=0x77A, did=0xF190)
        print(f"J533 serial: {serial.hex()}")
        print(f"VIN in J533: {vin.decode()}")

        # Enable raw sniff and watch what J533 sends during ignition cycle
        await bridge.set_raw_sniff(True)
        # ...cycle ignition, capture frames...
        await bridge.set_raw_sniff(False)

asyncio.run(probe())
```

---

## Credits

- Original firmware: [bri3d](https://github.com/bri3d) / [Switchleg1](https://github.com/Switchleg1)
- VW_Flash ecosystem: [bri3d/VW_Flash](https://github.com/bri3d/VW_Flash)
- C7 VAG research: [VAG-CP-Docs](https://github.com/dspl1236/VAG-CP-Docs)
- SA2 seed/key: [bri3d/sa2_seed_key](https://github.com/bri3d/sa2_seed_key)
