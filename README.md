# esp32-isotp-ble-bridge-c7vag

**Fork of [Switchleg1/esp32-isotp-ble-bridge](https://github.com/Switchleg1/esp32-isotp-ble-bridge)**  
Customised for C7 Audi A6/A7/A8 (VAG platform) with WiFi support and raw CAN sniff mode.

Part of the [simos-suite](https://github.com/dspl1236/simos-suite) open-source VAG diagnostic platform.

---

## Flash with FunkFlash-ESP

The easiest way to flash this firmware is with **[FunkFlash-ESP](https://github.com/dspl1236/FunkFlash-ESP)** —  
a one-click cross-platform flasher for Windows, macOS, and Linux.  
No Python, no ESP-IDF, no command line required.

**[→ Download FunkFlash-ESP](https://github.com/dspl1236/FunkFlash-ESP/releases/latest)**

---

## Firmware modes

### BLE mode (default)
- Advertises as `BLE_TO_ISOTP20` via Bluetooth
- BLE GATT service UUID: `0xABF0`
- Write characteristic (tester → ESP32): `0xABF1` — write-without-response
- Notify characteristic (ESP32 → tester): `0xABF2`
- Compatible with Simos Tools APK, simos-suite, and the VAG-CP PWA

### WiFi AP mode
- Creates open WiFi network: **FunkBridge**
- Captive portal: browser opens automatically on connect
- DNS server catches all queries → `192.168.4.1`
- Web app served from SPIFFS at `http://192.168.4.1`
- WebSocket bridge at `ws://192.168.4.1/ws`
- mDNS: `http://funkbridge.local`

### WiFi Station mode
- Joins existing WiFi network (credentials set via FunkFlash-ESP)
- mDNS: `http://funkbridge.local`
- Falls back to AP mode if network unavailable
- WebSocket bridge at `ws://funkbridge.local/ws`

**Note:** WiFi and BLE share one radio — they cannot run simultaneously.  
Use FunkFlash-ESP to switch between modes.

---

## Custom additions (vs upstream)

| Feature | Description |
|---------|-------------|
| `BRG_SETTING_RAW_SNIFF = 9` | Forward all raw CAN frames to BLE/WebSocket before ISO-TP filtering |
| `BLE_RAW_SNIFF_ID = 0xCAFE` | TX/RX ID used for raw sniff frames |
| `wifi_server.c/h` | Full WiFi subsystem — AP + station + captive portal + mDNS + WebSocket |
| `FUNKBRIDGE_VERSION` | Version string read by FunkFlash-ESP after flashing |
| C7 VAG CAN profile | Correct TX/RX IDs for J533, J255, J285, J234, J794, J136 |

---

## Packet framing

Every message is prefixed with an 8-byte header (`ble_header_t`):

```
Offset  Size  Field
0       1     hdID     — 0xF1 normal frame
1       1     cmdFlags — flag bits (BRG_COMMAND_FLAG_*)
2       2     rxID     — CAN RX address (little-endian)
4       2     txID     — CAN TX address (little-endian)
6       2     cmdSize  — payload length (little-endian)
[8...]        payload  — ISO-TP frame bytes
```

This format is identical over BLE and WebSocket —  
the same client code works for both transports.

---

## Build

Requires ESP-IDF v5.2.x:

```bash
git clone https://github.com/dspl1236/esp32-isotp-ble-bridge-c7vag
cd esp32-isotp-ble-bridge-c7vag
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

Or use the [GitHub Actions CI](.github/workflows/build-firmware.yml) which  
builds automatically on every push and attaches `.bin` files to releases.

---

## Hardware

**[→ Full wiring guide: docs/HARDWARE.md](docs/HARDWARE.md)**

Designed for **AITRIP ESP32-WROOM-32 + MCP2515 TJA1050 CAN module**:

| Channel | Interface | Speed | OBD-II pins |
|---------|-----------|-------|-------------|
| Drive Train CAN (J533) | TWAI + TJA1051T transceiver | 500 kbps | 6 / 14 |
| Convenience CAN (J255) | MCP2515 over SPI (TJA1050 on module) | 100 kbps | 3 / 11 |

**Required parts not in the Amazon kit:**
- TJA1051T or SN65HVD230 CAN transceiver (for TWAI channel)
- 10kΩ resistor from 3.3V to GPIO34 (MCP2515 INT pull-up — mandatory)

See [docs/HARDWARE.md](docs/HARDWARE.md) for complete pin assignments, assembly checklist, and first-connection test procedure.

---

## Related

- **[FunkFlash-ESP](https://github.com/dspl1236/FunkFlash-ESP)** — Cross-platform flasher for this firmware
- **[simos-suite](https://github.com/dspl1236/simos-suite)** — Desktop ECU diagnostic tool
- **[VAG-CP-Docs](https://github.com/dspl1236/VAG-CP-Docs)** — Component Protection research documentation
- **[FunkBridge PWA](https://dspl1236.github.io/FunkFlash-ESP/)** — Browser-based IKA key reader and diagnostic tool
- **[Switchleg1/esp32-isotp-ble-bridge](https://github.com/Switchleg1/esp32-isotp-ble-bridge)** — Upstream

---

## License

GPL v3 — Right to repair. Use freely.
