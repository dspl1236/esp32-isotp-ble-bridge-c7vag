# Hardware Assembly — AITRIP ESP32 + MCP2515 Dual-CAN Bridge

Specific wiring guide for building the **C7 VAG dual-CAN BLE bridge** using:
- **AITRIP ESP32-WROOM-32** (CP2102, 30-pin DevKit)
- **MCP2515 + TJA1050 CAN module** (8 MHz crystal, 2× 120Ω termination jumpers)
- **TJA1051T or SN65HVD230** CAN transceiver (for TWAI channel — see below)
- **10kΩ resistor** (pull-up for MCP2515 INT line)

---

## Architecture: Two CAN buses, one ESP32

| Channel | Bus | Speed | OBD-II Pins | Interface |
|---------|-----|-------|-------------|-----------|
| Drive Train CAN | J533 gateway | 500 kbps | 6 (+), 14 (−) | ESP32 TWAI (built-in) + external TJA1051T |
| Convenience CAN | J255 HVAC | 100 kbps | 3 (+), 11 (−) | MCP2515 over SPI (TJA1050 built into module) |

Both channels run as independent FreeRTOS tasks and share the same BLE/WiFi transport.
Commands from simos-suite with flag `0x10` set route to the MCP2515 channel.

---

## Parts list

| Part | Source | Notes |
|------|--------|-------|
| AITRIP 3PCS ESP32-WROOM-32 (CP2102, 30-pin) | Amazon | VID `0x10C4` PID `0xEA60` |
| WWZMDiB MCP2515 + TJA1050 CAN module | Amazon | 8 MHz crystal, has built-in 120Ω termination jumper |
| **TJA1051T** (SOIC-8) or **SN65HVD230** (SOIC-8) | DigiKey / Mouser / eBay | For TWAI channel — **required**, not optional |
| 10kΩ resistor (0805 or through-hole) | Any | GPIO34 pull-up — **required** |
| OBD-II breakout / pigtail | eBay | Or tap directly at J533 connector under dash |

> **Why you need the TJA1051T:** The ESP32's TWAI controller outputs bare 3.3V CMOS logic.
> A CAN transceiver converts that to the differential CAN-H/CAN-L bus signalling the car expects.
> The MCP2515 module already has a TJA1050 for its own channel — the TWAI channel needs its own.

---

## Pin assignments

### TWAI — Drive Train CAN (J533, 500 kbps)

```
ESP32 GPIO5  ──→  TJA1051T TXD  (pin 1)
ESP32 GPIO4  ←──  TJA1051T RXD  (pin 4)
ESP32 GPIO21 ──→  TJA1051T STB  (pin 8)   ← pulled LOW by firmware = active mode
3.3V         ──→  TJA1051T VCC  (pin 3)
GND          ──→  TJA1051T GND  (pin 2)
TJA1051T CANH (pin 7) ──→  OBD-II pin 6
TJA1051T CANL (pin 6) ──→  OBD-II pin 14
```

GPIO5 is a strapping pin. TJA1051T RXD idles HIGH (recessive) so it is safe to use —
the ESP32 will boot correctly with it connected.

### MCP2515 — Convenience CAN (J255, 100 kbps)

```
ESP32 GPIO18 ──→  MCP2515 module SCK
ESP32 GPIO23 ──→  MCP2515 module MOSI (SI)
ESP32 GPIO19 ←──  MCP2515 module MISO (SO)
ESP32 GPIO17 ──→  MCP2515 module CS
ESP32 GPIO34 ←──  MCP2515 module INT    ← see pull-up note below!
3.3V         ──→  MCP2515 module VCC    (module is 3.3V-compatible)
GND          ──→  MCP2515 module GND
MCP2515 module CANH ──→  OBD-II pin 3
MCP2515 module CANL ──→  OBD-II pin 11
```

### ⚠️ Critical: GPIO34 pull-up resistor

**GPIO34 has no internal pull-up at the silicon level** — it is an input-only GPIO.
The MCP2515 INT pin is active-low open-drain. Without a pull-up it floats and fires
spurious interrupts constantly, making the MCP2515 channel unreliable.

```
3.3V ──[10kΩ]──┬── ESP32 GPIO34
               └── MCP2515 INT
```

Add a 10kΩ resistor between 3.3V and GPIO34 **before powering up**.

---

## MCP2515 module notes

The MCP2515 module runs at **100 kbps** configured for an **8 MHz crystal**:

| Register | Value | Meaning |
|----------|-------|---------|
| CNF1 | `0x03` | BRP=3 → TQ = 1 µs |
| CNF2 | `0x92` | PRSEG=2, PHSEG1=3, SAM=1 |
| CNF3 | `0x02` | PHSEG2=3 |

Total NTQ = 1+3+4+3 = 11 → 1 / (11 × 1 µs) ≈ **100 kbps** ✓

**Termination:** The MCP2515 module has a 120Ω resistor with a solder-jumper.
The C7 Convenience CAN bus (J255) already has termination in the vehicle harness.
**Remove the 120Ω jumper on the module** unless you are testing on a bench without
a car connected.

---

## TJA1051T vs TJA1050 vs SN65HVD230

| Part | VCC | Standby pin | Notes |
|------|-----|-------------|-------|
| TJA1051T | 3.3V or 5V | STB (active-low) | Preferred — VAG factory part family, 3.3V native |
| TJA1050 | 5V only | None | Already on MCP2515 module. Don't use for TWAI if running 3.3V |
| SN65HVD230 | 3.3V | RS (pull LOW for normal mode) | Good substitute for TJA1051T |

If using SN65HVD230: connect RS pin to GND (not GPIO21). Change `SILENT_GPIO_NUM`
in `constants.h` or tie GPIO21 to GND as well — the firmware pulls it LOW on init
which is correct for both parts.

---

## OBD-II connector pinout reference (C7 A6/A7)

```
OBD-II pin  Signal              ECU / Module
──────────  ──────────────────  ─────────────────────────────
2           K-Line              Legacy KWP2000 (not used here)
3           Convenience CAN H   J255, J285, J234, J794 (100 kbps)
4           Chassis GND
5           Signal GND
6           Drive Train CAN H   J533, J17, J623, ZF8HP (500 kbps)
11          Convenience CAN L   (paired with pin 3)
14          Drive Train CAN L   (paired with pin 6)
16          Battery +12V
```

Both CAN buses use **120Ω termination** at each end of the bus in the vehicle.
Do not add extra termination when connecting in-circuit to a running car.

---

## Assembly checklist

- [ ] 10kΩ resistor soldered/wired from 3.3V to GPIO34
- [ ] TJA1051T wired: GPIO5→TXD, GPIO4→RXD, GPIO21→STB, 3.3V/GND, CANH/CANL to OBD pins 6/14
- [ ] MCP2515 module SPI wired: GPIO18/23/19/17/34 to SCK/MOSI/MISO/CS/INT
- [ ] MCP2515 module CANH/CANL to OBD pins 3/11
- [ ] MCP2515 120Ω termination jumper removed (in-car use)
- [ ] OBD-II pin 16 → ESP32 VIN (or use USB power from laptop)
- [ ] OBD-II pins 4+5 → ESP32 GND

---

## Flashing firmware

Use the prebuilt binary from the GitHub releases — no ESP-IDF required:

```
pip install esptool
esptool.py --chip esp32 --port COM3 --baud 460800 \
  write_flash -z \
  0x1000  bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 firmware.bin \
  0x310000 spiffs.bin
```

Or use [FunkFlash-ESP](https://github.com/dspl1236/FunkFlash-ESP) for one-click flashing.

After flashing, the ESP32 advertises as `BLE_TO_ISOTP20` and is ready for simos-suite.

---

## First connection test

With the bridge powered and BLE connected in simos-suite:

1. **Connect tab** → select `BLE_TO_ISOTP20` → Connect
2. **ECU Info tab** → Read — should wake J533 and return 18 standard DIDs
3. Check the simos-suite log for `3E 80` wakeup frame on TX, response on RX

If the Drive Train CAN channel is silent, check:
- TJA1051T STB pin is LOW (GPIO21 pulled low by firmware)
- GPIO5/4 wiring to TXD/RXD not swapped
- OBD-II pins 6/14 not swapped

If the MCP2515 channel fails to init:
- Check SPI wiring (MOSI/MISO are commonly swapped)
- Verify 10kΩ pull-up on GPIO34
- Check MCP2515 VCC is 3.3V (some modules have 5V-only markings but work at 3.3V with the TJA1050 floating — power from 3.3V rail only)
