# FunkBridge KWP2000 / K-Line Hardware Variant

**Reference document for future development.**
This describes what is needed to extend FunkBridge with K-line (ISO 14230-4)
physical layer support alongside the existing CAN/ISO-TP bridge.

---

## Why KWP2000 over K-line

The current FunkBridge firmware handles KWP2000 **over CAN** transparently —
J533 acts as a gateway and translates internally. This covers all C7 A6/A7/A8
modules for CP work without any hardware changes.

K-line support becomes relevant for:

- **Pre-2003 VAG vehicles** — before CAN was standard (Golf IV, Passat B5,
  Audi A4 B5/B6 etc.) where KWP2000 is the primary diagnostic protocol
- **Direct module access** — bypassing J533 gateway to reach modules directly
  on the K-line bus when gateway routing is unavailable
- **Legacy ECU tuning** — older ME7.x / EDC15 / EDC16 ECUs that communicate
  exclusively via K-line
- **Immobiliser work on older platforms** — pre-CAN IMMO units accessible
  only via K-line pin 7

---

## Physical Layer — What K-line Is

K-line is a single-wire half-duplex serial bus on OBD-II pin 7 (and sometimes
pin 15 for L-line). Electrical characteristics:

| Parameter      | Value                              |
|----------------|------------------------------------|
| Voltage levels | 12V logic (not 3.3V or 5V TTL)     |
| Idle state     | High (12V)                         |
| Baud rate      | 1200–10400 bps (typically 10400)   |
| Init sequence  | 5-baud init or fast init (ISO 14230)|
| Direction      | Half-duplex — one talker at a time |

The ESP32 GPIO is 3.3V logic. A dedicated transceiver IC is **required** —
you cannot connect K-line directly to a GPIO pin.

---

## Transceiver Options

### Option A — L9637D (ST Microelectronics) — Recommended

The same chip used in many OEM ECUs and diagnostic interfaces.

- Supply: 5–40V (powered from OBD pin 16 — battery voltage)
- Tx input: 5V or 3.3V compatible
- Rx output: 5V open-collector (needs pull-up, use voltage divider to 3.3V)
- Package: SO-8 or DIP-8 — easy to hand-solder
- Cost: ~$0.80

**Wiring:**

```
OBD pin 7 (K-line) ───────────────── L9637 pin 2 (KBUS)
OBD pin 16 (12V)  ─── 100Ω ──────── L9637 pin 8 (VCC)
GND               ────────────────── L9637 pin 5 (GND)
ESP32 GPIO (TX)   ─── level shift ── L9637 pin 1 (TxD)
ESP32 GPIO (RX)   ─── volt divider ─ L9637 pin 4 (RxD)
```

L9637 RxD output is 5V — use a 10kΩ/20kΩ voltage divider before ESP32 GPIO.
For TX: BSS138 N-channel MOSFET with 10kΩ pull-up to 5V works cleanly.

### Option B — MC33290 (NXP)

Very similar to L9637, slightly different pinout. Also widely used.
Datasheet: https://www.nxp.com/docs/en/data-sheet/MC33290.pdf

### Option C — TH3122 (Melexis)

Higher integration — includes 5-baud init pattern generator in hardware.
More expensive and harder to source; not necessary for software-driven init.

---

## ESP32 Hardware Connections

Suggested GPIO assignments for a K-line capable FunkBridge board:

```
Function          GPIO    Notes
─────────────────────────────────────────────────────
CAN TX            GPIO 5  Existing (TWAI)
CAN RX            GPIO 4  Existing (TWAI)
K-line TX         GPIO 17 UART2 TxD — through L9637
K-line RX         GPIO 16 UART2 RxD — from L9637 via divider
CAN Silent        GPIO 21 Existing
LED               GPIO 2  Existing
L9637 enable      GPIO 33 Pull high to enable (optional)
```

UART2 is the natural choice — UART0 is used for debug/flash, UART1 may be
in use by existing firmware. The ESP32 has three hardware UARTs.

---

## Firmware Architecture

### New files: `main/kwp_bridge.c` + `main/kwp_bridge.h`

Mirrors the existing `isotp_bridge.c` architecture but for K-line:

```c
void kwp_init(void);
void kwp_deinit(void);
void kwp_start_task(void);
void kwp_stop_task(void);

// ISO 14230-4 fast init: sends 0xC1 0x33 0xF1 0x81, waits for KeyBytes
esp_err_t kwp_fast_init(uint8_t target_addr);

// 5-baud init for older ECUs
esp_err_t kwp_5baud_init(uint8_t target_addr);

// Send KWP2000 request, receive response
// Handles header, length, checksum, P2 timer
esp_err_t kwp_request(const uint8_t *req, size_t req_len,
                       uint8_t *resp, size_t *resp_len,
                       uint32_t timeout_ms);
```

### Frame routing over BLE/WebSocket

Use a dedicated txID/rxID range to distinguish KWP frames from ISO-TP:

```
txID = 0xEE00–0xEEFF  (KWP request  — tester to ECU)
rxID = 0xEF00–0xEFFF  (KWP response — ECU to tester)
Payload = raw KWP2000 frame bytes
```

This is cleaner than a flag bit — consistent with how ISO-TP channels
are identified, no ambiguity.

### K-line timing requirements (critical)

KWP2000 has strict inter-byte and inter-frame timing. Must use hardware
timer accuracy, not vTaskDelay:

| Timer | Meaning                          | Value         |
|-------|----------------------------------|---------------|
| P1    | Inter-byte time (ECU to tester)  | 0–20ms        |
| P2    | ECU response time                | 0–50ms        |
| P3    | Inter-request time               | 55ms minimum  |
| P4    | Inter-byte time (tester to ECU)  | 5ms minimum   |
| W1    | 5-baud: time after address byte  | 60–300ms      |
| W2    | 5-baud: time before sync byte    | 5–20ms        |
| W3    | 5-baud: time before key byte 2   | 0–20ms        |
| W4    | 5-baud: complement response time | 25–50ms       |
| W5    | Time between init attempts       | 300ms minimum |

Use `esp_timer_get_time()` for microsecond precision.

---

## FunkFlash-ESP Changes

### New firmware mode: KWP Bridge

Add a fourth mode to the FunkFlash-ESP UI alongside BLE / WiFi AP / WiFi STA:

```
Firmware mode:
  BLE          mobile / Bluetooth
  WiFi AP      instant bench
  WiFi Station join your network
  KWP Bridge   K-line pin 7 + CAN (requires hardware mod)
```

The KWP firmware would run both TWAI (CAN) and K-line UART simultaneously.
Frame routing is by txID range — ISO-TP IDs to CAN, KWP IDs to K-line.

### Hardware detection in FunkFlash-ESP

After flashing, read the version string from the device. The KWP firmware
advertises a different suffix:

```
FUNKBRIDGE_VERSION = "1.1.0-KWP"
```

If the suffix is absent, grey out the KWP firmware option. This prevents
someone from flashing KWP firmware onto hardware without the L9637 circuit.

---

## simos-suite transport layer

A new `transport/kwp_bridge.py` alongside `ws_bridge.py`:

```python
class KWPBridge:
    # KWP2000 client over FunkBridge WebSocket or BLE
    # Uses KWP txID range (0xEE00+) for frame routing

    def fast_init(self, target: int = 0x01) -> bool:
        # Send fast init frame, return True if ECU responded

    def request(self, service: int, *data: int) -> bytes:
        # Send KWP service request, return response data bytes

    def read_ecu_id(self) -> dict:
        # Service 0x1A — read ECU identification

    def read_dtcs(self) -> list:
        # Service 0x18 — read diagnostic trouble codes

    def clear_dtcs(self) -> None:
        # Service 0x14 — clear diagnostic information

    def read_data_by_local_id(self, lid: int) -> bytes:
        # Service 0x21 — read data by local identifier

    def read_data_by_common_id(self, cid: int) -> bytes:
        # Service 0x22 — read data by common identifier (same as UDS)

    def write_data_by_local_id(self, lid: int, data: bytes) -> None:
        # Service 0x3B — write data by local identifier
```

---

## Recommended board revision

A FunkBridge-KWP PCB adds to the existing A0 design:

```
FunkBridge-KWP additions vs A0:
  + L9637D transceiver (SO-8, ~$0.80)
  + BSS138 level shifter for TxD
  + 10k/20k voltage divider for RxD
  + Route OBD pin 7 to L9637
  + GPIO 16/17 routed to UART2

OBD-II connector wiring:
  pin 4  — Chassis GND
  pin 5  — Signal GND
  pin 6  — CAN High (J2284)
  pin 7  — K-line           ← add connection
  pin 14 — CAN Low (J2284)
  pin 16 — Battery +12V
```

PCB change is minimal — three new components, two new signal routes.

---

## Existing KWP2000 Python libraries

- `python-obd` — OBD-focused, has KWP but not VAG-specific
- `kwp2000` on PyPI — clean implementation, worth reviewing
- Custom implementation is straightforward given the ISO 14230 spec and
  VAG's published extensions in ETKA/ELSA engineering documentation

---

## Reference documents

- ISO 14230-1 — KWP2000 Physical layer
- ISO 14230-2 — KWP2000 Data link layer
- ISO 14230-3 — KWP2000 Application layer
- ISO 14230-4 — KWP2000 Emission-related requirements
- L9637D datasheet — ST Microelectronics
- MC33290 datasheet — NXP
- ESP32 Technical Reference Manual — UART chapter (section 13)

---

## Implementation order (when ready)

1. Build L9637 transceiver circuit on breadboard
2. Verify K-line idle/active levels with oscilloscope
3. Implement `kwp_init()` + `kwp_5baud_init()` in ESP-IDF
4. Test against a known ECU on the bench (old IMMO unit or ME7 ECU)
5. Implement `kwp_request()` with correct P2/P3/P4 timing
6. Add KWP txID range routing in `isotp_bridge.c`
7. Add `kwp_bridge.py` transport to simos-suite
8. Add KWP mode to FunkFlash-ESP firmware selector
9. Design FunkBridge-KWP PCB revision

---

*Status: planning — no implementation started.*
*Target: ESP32 A0 + L9637D daughter board, or revised PCB.*
*Repo: dspl1236/esp32-isotp-ble-bridge-c7vag*
