# Hardware Assembly — AITRIP ESP32 + MCP2515 Dual-CAN Bridge

Specific wiring guide for building the **C7 VAG dual-CAN BLE bridge** using:
- **AITRIP ESP32-WROOM-32** (CP2102, 30-pin DevKit)
- **MCP2515 + TJA1050 CAN module** (8 MHz crystal, 120Ω termination jumper)
- **SN65HVD230 breakout module** (purple PCB, CTX/CRX/S/CANH/CANL labels) — confirmed working
- **10kΩ resistor** (pull-up for MCP2515 INT line — mandatory)

> **Transceiver note:** The purple SN65HVD230 breakout module (pin labels: VCC GND CTX CRX CANH CANL S NC)
> is the confirmed part for TWAI/Drive Train CAN. The TJA1051T is an equivalent substitute if you have one.
> Pin names differ between parts — see the wiring tables below for each.

---

## Architecture: Two CAN buses, one ESP32

| Channel | Bus | Speed | OBD-II Pins | Interface |
|---------|-----|-------|-------------|-----------|
| Drive Train CAN | J533 gateway | 500 kbps | 6 (+), 14 (−) | ESP32 TWAI (built-in) + SN65HVD230 |
| Convenience CAN | J255 HVAC | 100 kbps | 3 (+), 11 (−) | MCP2515 over SPI (TJA1050 built into module) |

Both channels run as independent FreeRTOS tasks and share the same BLE/WiFi transport.
Commands from simos-suite with flag `0x10` set route to the MCP2515 channel.

---

## Parts list

| Part | Notes |
|------|-------|
| AITRIP 3PCS ESP32-WROOM-32 (CP2102, 30-pin) | VID `0x10C4` PID `0xEA60` |
| WWZMDiB MCP2515 + TJA1050 CAN module | 8 MHz crystal, built-in 120Ω termination jumper |
| **SN65HVD230 breakout** (purple PCB) | TWAI channel transceiver — **confirmed working** |
| 10kΩ resistor (0805 or through-hole) | GPIO34 pull-up — **required, do not skip** |
| **EBOOT MP1584EN** buck converter (6-pack) | OBD +12V → 5V regulated — **set to 5V before connecting** |
| OBD-II pigtail or male connector | iKKEGOL J1962 pigtail or OLLGEN male connector |

---

## Pin assignments

### TWAI — Drive Train CAN (J533, 500 kbps) using SN65HVD230

```
ESP32 GPIO5  ──→  SN65HVD230 CTX   ← TWAI transmit output
ESP32 GPIO4  ←──  SN65HVD230 CRX   ← TWAI receive input
GND          ──→  SN65HVD230 S     ← tie LOW = high-speed mode (500 kbps)
3.3V         ──→  SN65HVD230 VCC
GND          ──→  SN65HVD230 GND
             ──   SN65HVD230 NC    ← leave unconnected
SN65HVD230 CANH ──→  OBD-II pin 6
SN65HVD230 CANL ──→  OBD-II pin 14
```

> **S pin:** Tie directly to GND on the breadboard for normal high-speed operation.
> GPIO21 is not needed for the SN65HVD230 — the firmware pulls GPIO21 LOW anyway
> which is harmless, but wiring S to GND directly is simpler and more reliable.

> **GPIO5 strapping pin:** Safe to use — SN65HVD230 CRX idles HIGH (recessive bus)
> so the ESP32 boot state is not affected.

### TWAI — Drive Train CAN using TJA1051T (alternate part)

```
ESP32 GPIO5  ──→  TJA1051T TXD  (pin 1)
ESP32 GPIO4  ←──  TJA1051T RXD  (pin 4)
ESP32 GPIO21 ──→  TJA1051T STB  (pin 8)   ← firmware pulls LOW = active mode
3.3V         ──→  TJA1051T VCC  (pin 3)
GND          ──→  TJA1051T GND  (pin 2)
TJA1051T CANH (pin 7) ──→  OBD-II pin 6
TJA1051T CANL (pin 6) ──→  OBD-II pin 14
```

### MCP2515 — Convenience CAN (J255, 100 kbps)

```
ESP32 GPIO18 ──→  MCP2515 module SCK
ESP32 GPIO23 ──→  MCP2515 module MOSI (SI)
ESP32 GPIO19 ←──  MCP2515 module MISO (SO)
ESP32 GPIO17 ──→  MCP2515 module CS
ESP32 GPIO34 ←──  MCP2515 module INT    ← see pull-up note below!
5V           ──→  MCP2515 module VCC   ← 5V from MP1584EN, NOT 3.3V (TJA1050 is 5V-only)
GND          ──→  MCP2515 module GND
MCP2515 module CANH ──→  OBD-II pin 3
MCP2515 module CANL ──→  OBD-II pin 11
```

### ⚠️ Critical: GPIO34 pull-up resistor

**GPIO34 is input-only at the silicon level — no internal pull-up exists.**
Confirmed from the ESP32 pinout: GPIO34 is labeled "Input only / RTC GPIO4 / ADC1_6."
The MCP2515 INT pin is active-low open-drain. Without a pull-up it floats high/low
unpredictably and fires spurious interrupts constantly.

```
3.3V ──[10kΩ]──┬── ESP32 GPIO34
               └── MCP2515 INT
```

Add the 10kΩ resistor between 3.3V and GPIO34 **before powering up.**

---

## ESP32 GPIO warnings

| GPIO | Issue | Detail |
|------|-------|--------|
| GPIO34 | **Input-only, no pull-up** | Use for MCP2515 INT only — requires external 10kΩ to 3.3V |
| GPIO35 | Input-only, no pull-up | Avoid unless needed |
| GPIO36/39 | Input-only | ADC use only |
| **GPIO12** | **Strapping pin** | If HIGH at boot → selects 1.8V flash voltage → ESP32 won't boot. Do not connect anything to GPIO12 on your breadboard. |
| GPIO5 | Strapping pin | Safe with SN65HVD230/TJA — transceiver CRX/RXD idles HIGH (recessive), boot not affected |
| GPIO0 | Strapping pin | Hold LOW to enter flash mode. Leave floating or pull HIGH for normal boot. |
| GPIO2 | Strapping pin (LED) | On-board LED, must be LOW at boot. Already handled by Switchleg firmware. |
| GPIO6–11 | **Flash memory** | Hard-wired to SPI flash. Never use these. |

---

## Transceiver comparison

| Part | VCC | Mode pin | Pin names | Notes |
|------|-----|----------|-----------|-------|
| **SN65HVD230** | 3.3V | RS → GND for high-speed | CTX / CRX / CANH / CANL / S | **Confirmed working. Purple breakout module.** |
| TJA1051T | 3.3V or 5V | STB → LOW for active | TXD / RXD / CANH / CANL / STB | VAG factory part family, direct substitute |
| TJA1050 | **5V only** | None | TXD / RXD / CANH / CANL | Already on MCP2515 module. Do not use for TWAI at 3.3V. |

---

## MCP2515 bit timing (100 kbps, 8 MHz crystal)

| Register | Value | Meaning |
|----------|-------|---------|
| CNF1 | `0x03` | BRP=3 → TQ = 1 µs |
| CNF2 | `0x92` | PRSEG=2, PHSEG1=3, SAM=1 |
| CNF3 | `0x02` | PHSEG2=3 |

Total NTQ = 11 → 1 / (11 × 1 µs) = **100 kbps** ✓

**Termination:** Remove the 120Ω solder-jumper on the MCP2515 module for in-car use.
The C7 CAN buses have 120Ω termination built into the vehicle harness already.

---

## OBD-II connector pinout (C7 A6/A7)

```
OBD-II pin  Signal              ECU / Module
──────────  ──────────────────  ─────────────────────────────
3           Convenience CAN H   J255, J285, J234, J794 (100 kbps)
4           Chassis GND
5           Signal GND
6           Drive Train CAN H   J533, J17, J623, ZF8HP (500 kbps)
11          Convenience CAN L   (paired with pin 3)
14          Drive Train CAN L   (paired with pin 6)
16          Battery +12V
```

---

## Power — MP1584EN Buck Converter (OBD +12V → 5V)

The car's OBD-II port supplies **12–14.4V** on pin 16. The ESP32 and MCP2515 module
need regulated 5V. The **EBOOT MP1584EN** mini buck converter handles this.

```
OBD-II pin 16 (+12V) ──→  MP1584EN  IN+
OBD-II pin 4 or 5 (GND) ──→  MP1584EN  IN−
MP1584EN  OUT+ ──→  ESP32 VIN   (5V)
MP1584EN  OUT+ ──→  MCP2515 module VCC   (5V)
MP1584EN  OUT− ──→  common GND
```

The ESP32's onboard 3.3V LDO converts 5V → 3.3V for itself and the SN65HVD230.

### ⚠️ Set output voltage BEFORE connecting anything

The MP1584EN ships with an unknown output voltage. **Do not connect it to the ESP32
until you have verified 5V output.**

1. Power the MP1584EN from any 7–24V source (a USB charger won't work — needs >5V in)
2. Probe OUT+ and OUT− with a multimeter
3. Turn the small brass trim pot until the output reads **5.0V**
4. Then wire to ESP32 VIN and MCP2515 VCC

### Voltage rails summary

| Rail | Source | Powers |
|------|--------|--------|
| 12–14V | OBD pin 16 | MP1584EN input |
| **5V** | MP1584EN OUT+ | ESP32 VIN, MCP2515 module VCC |
| **3.3V** | ESP32 onboard LDO | SN65HVD230 VCC, logic signals |
| GND | OBD pins 4+5 | Common ground for everything |

> The SN65HVD230 is 3.3V — power it from ESP32 3.3V pin, **not** the 5V rail.
> The MCP2515 module is 5V — power it from MP1584EN OUT+, **not** 3.3V.

---

## Assembly checklist

- [ ] 10kΩ resistor wired from 3.3V to GPIO34 (MCP2515 INT pull-up)
- [ ] SN65HVD230: CTX→GPIO5, CRX→GPIO4, S→GND, VCC→3.3V, GND→GND
- [ ] SN65HVD230: CANH→OBD pin 6, CANL→OBD pin 14
- [ ] MCP2515: SCK→GPIO18, MOSI→GPIO23, MISO→GPIO19, CS→GPIO17, INT→GPIO34
- [ ] MCP2515: CANH→OBD pin 3, CANL→OBD pin 11
- [ ] MCP2515: 120Ω termination jumper removed
- [ ] Nothing connected to GPIO12 on the breadboard
- [ ] MP1584EN output trimmed to **5.0V** (verify with multimeter before connecting)
- [ ] OBD-II pin 16 → MP1584EN IN+
- [ ] OBD-II pin 4 or 5 → MP1584EN IN− (GND)
- [ ] MP1584EN OUT+ → ESP32 VIN
- [ ] MP1584EN OUT+ → MCP2515 module VCC
- [ ] MP1584EN OUT− → common GND
- [ ] SN65HVD230 VCC → ESP32 3.3V pin (not 5V)

---

## Flashing firmware

Use [FunkFlash-ESP](https://github.com/dspl1236/FunkFlash-ESP) for one-click flashing (recommended),
or manually with esptool:

```
pip install esptool
esptool.py --chip esp32 --port COM3 --baud 460800 \
  write_flash -z \
  0x1000  bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 firmware.bin \
  0x310000 spiffs.bin
```

After flashing the ESP32 advertises as `BLE_TO_ISOTP20` and is ready for simos-suite.

---

## First connection test

1. **Connect tab** → select `BLE_TO_ISOTP20` → Connect
2. **ECU Info tab** → Read — should return 18 standard DIDs from J533
3. Check simos-suite log for `3E 80` wakeup frame on TX, response on RX

**Drive Train CAN silent?**
- Check SN65HVD230 S pin is tied to GND
- Verify GPIO5→CTX and GPIO4→CRX not swapped
- Verify OBD pins 6/14 not swapped

**MCP2515 fails to init?**
- Verify 10kΩ pull-up on GPIO34
- Check MOSI/MISO not swapped (most common mistake)
- Verify MCP2515 VCC is on **5V rail** (from MP1584EN OUT+), not 3.3V — TJA1050 is 5V-only
