"""
ble_bridge_client.py — async BLE client for esp32-isotp-ble-bridge (C7 VAG fork)

Implements the 0xF1 BLE header protocol as defined in ble_server.h.
Works with both C7_VAG profile (8 containers) and MQB profile (4 containers).

Header format (8 bytes, little-endian 16-bit IDs):
  [0]    0xF1        magic
  [1]    cmdFlags    control flags
  [2:4]  rxID        CAN ID we expect responses on  (little-endian)
  [4:6]  txID        CAN ID we send to              (little-endian)
  [6:8]  cmdSize     payload byte count             (little-endian)
  [8:]   payload     UDS bytes

Response from bridge (same header, reversed IDs):
  txID = ECU's tx (= our rxID)
  rxID = our tx   (= ECU's rx)
  payload = raw UDS response bytes
"""

import asyncio
import struct
import logging
from typing import Optional, Callable, AsyncIterator
from bleak import BleakClient, BleakScanner

log = logging.getLogger(__name__)

# BLE GATT UUIDs (SPP-style service used by BridgeLEG)
SPP_SERVICE_UUID      = "0000abf0-0000-1000-8000-00805f9b34fb"
SPP_DATA_RECV_UUID    = "0000abf1-0000-1000-8000-00805f9b34fb"  # write (host→bridge)
SPP_DATA_NOTIFY_UUID  = "0000abf2-0000-1000-8000-00805f9b34fb"  # notify (bridge→host)

# Protocol constants
BLE_HEADER_ID       = 0xF1
BLE_PARTIAL_ID      = 0xF2
BLE_RAW_SNIFF_ID    = 0xCAFE   # magic txID/rxID used for raw frame forwarding

# cmdFlags bits
FLAG_PER_ENABLE     = 0x01  # start persist/poll mode
FLAG_PER_CLEAR      = 0x02  # clear stored persist messages
FLAG_PER_ADD        = 0x04  # add this message to persist list
FLAG_SPLIT_PK       = 0x08  # split/fragmented packet
FLAG_SETTINGS_GET   = 0x40  # settings read/write
FLAG_SETTINGS       = 0x80  # combined with 0x40

# Settings IDs
SETTING_ISOTP_STMIN     = 1
SETTING_LED_COLOR       = 2
SETTING_PERSIST_DELAY   = 3
SETTING_PERSIST_Q_DELAY = 4
SETTING_BLE_SEND_DELAY  = 5
SETTING_BLE_MULTI_DELAY = 6
SETTING_PASSWORD        = 7
SETTING_GAP             = 8
SETTING_RAW_SNIFF       = 9   # C7 VAG fork addition

# ─── C7 VAG CAN ID map ───────────────────────────────────────────────────────
C7_MODULES = {
    "J533_GATEWAY":     (0x710, 0x77A),
    "J255_HVAC":        (0x746, 0x7B0),
    "J136_SEAT_DRV":    (0x74C, 0x7B6),
    "J521_SEAT_PASS":   (0x74D, 0x7B7),
    "ECU_ENGINE":       (0x7E0, 0x7E8),   # Simos8.5 / 3.0T TFSI
    "TCU_ZF8HP":        (0x7E1, 0x7E9),
    "J104_ABS":         (0x760, 0x768),
    "SPARE_DTC":        (0x700, 0x7E8),
}

MQB_MODULES = {
    "ECU_SIMOS18":  (0x7E0, 0x7E8),
    "TCU_DQ250":    (0x7E1, 0x7E9),
    "HALDEX":       (0x7E5, 0x7ED),
    "DTC_BCAST":    (0x700, 0x7E8),
}


def build_header(tx_id: int, rx_id: int, payload_len: int, flags: int = 0) -> bytes:
    return struct.pack("<BBHHHH",
        BLE_HEADER_ID,
        flags,
        rx_id & 0xFFFF,
        tx_id & 0xFFFF,
        payload_len,
    # wait — header is 8 bytes: hdID(1) cmdFlags(1) rxID(2) txID(2) cmdSize(2)
    )[:-2] + struct.pack("<H", payload_len)


def build_packet(tx_id: int, rx_id: int, payload: bytes, flags: int = 0) -> bytes:
    hdr = struct.pack("<BBHHH",
        BLE_HEADER_ID,
        flags,
        rx_id & 0xFFFF,
        tx_id & 0xFFFF,
        len(payload),
    )
    return hdr + payload


def parse_header(data: bytes) -> Optional[dict]:
    if len(data) < 8 or data[0] != BLE_HEADER_ID:
        return None
    _, flags, rx_id, tx_id, cmd_size = struct.unpack_from("<BBHHH", data, 0)
    return {
        "flags":    flags,
        "rx_id":    rx_id,
        "tx_id":    tx_id,
        "cmd_size": cmd_size,
        "payload":  data[8:8 + cmd_size],
    }


class RawFrame:
    """A decoded raw CAN frame received during sniff mode."""
    def __init__(self, raw: bytes, timestamp_ms: Optional[int] = None):
        self.can_id  = (raw[0] << 8) | raw[1]
        self.dlc     = raw[2]
        self.data    = raw[3:3 + self.dlc]
        self.timestamp_ms = timestamp_ms

    def __repr__(self):
        return (f"CAN id=0x{self.can_id:03X} dlc={self.dlc} "
                f"data={self.data.hex(' ').upper()}"
                + (f" t={self.timestamp_ms}ms" if self.timestamp_ms else ""))


class BLEBridgeClient:
    """
    Async context-manager client for the BridgeLEG firmware.

    Usage:
        async with BLEBridgeClient(device_name="BLE_TO_ISOTP20") as bridge:
            resp = await bridge.uds_request(tx=0x710, rx=0x77A,
                                            payload=bytes([0x22, 0xF1, 0x8C]))
            print(resp.hex())
    """

    def __init__(self,
                 device_address: Optional[str] = None,
                 device_name: str = "BLE_TO_ISOTP20",
                 timeout: float = 10.0):
        self._address     = device_address
        self._name        = device_name
        self._timeout     = timeout
        self._client:     Optional[BleakClient] = None
        self._rx_queue:   asyncio.Queue = asyncio.Queue()
        self._sniff_queue: asyncio.Queue = asyncio.Queue()
        self._raw_sniff_cb: Optional[Callable] = None
        self._write_char  = None
        self._notify_char = None

    # ── Context manager ───────────────────────────────────────────────────────
    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, *args):
        await self.disconnect()

    # ── Connection ────────────────────────────────────────────────────────────
    async def connect(self):
        if not self._address:
            log.info("Scanning for %s ...", self._name)
            device = await BleakScanner.find_device_by_name(
                self._name, timeout=self._timeout)
            if not device:
                raise RuntimeError(f"Device '{self._name}' not found")
            self._address = device.address

        log.info("Connecting to %s ...", self._address)
        self._client = BleakClient(self._address)
        await self._client.connect(timeout=self._timeout)
        log.info("Connected")

        # Subscribe to notify characteristic
        await self._client.start_notify(SPP_DATA_NOTIFY_UUID, self._on_notify)

    async def disconnect(self):
        if self._client and self._client.is_connected:
            await self._client.disconnect()

    # ── Notification handler ──────────────────────────────────────────────────
    def _on_notify(self, _char, data: bytearray):
        data = bytes(data)

        if len(data) < 8 or data[0] != BLE_HEADER_ID:
            return  # unexpected / partial

        _, flags, rx_id, tx_id, cmd_size = struct.unpack_from("<BBHHH", data, 0)
        payload = data[8:8 + cmd_size]

        # Raw sniff frame? (magic sentinel IDs)
        if tx_id == BLE_RAW_SNIFF_ID and rx_id == BLE_RAW_SNIFF_ID:
            frame = RawFrame(payload)
            self._sniff_queue.put_nowait(frame)
            if self._raw_sniff_cb:
                self._raw_sniff_cb(frame)
            return

        # Persist mode response?  (rxID and txID carry a timestamp, not CAN IDs)
        # Detect by checking if values are in a plausible CAN ID range
        # Otherwise treat as normal UDS response and put in rx_queue
        self._rx_queue.put_nowait({"tx_id": tx_id, "rx_id": rx_id, "payload": payload})

    # ── Low-level write ───────────────────────────────────────────────────────
    async def _write(self, data: bytes):
        await self._client.write_gatt_char(SPP_DATA_RECV_UUID, data, response=False)

    # ── UDS request/response ──────────────────────────────────────────────────
    async def uds_request(self, tx: int, rx: int,
                           payload: bytes, timeout: float = 5.0) -> bytes:
        """Send a raw UDS payload and return the response payload bytes."""
        # Flush stale responses
        while not self._rx_queue.empty():
            self._rx_queue.get_nowait()

        packet = build_packet(tx, rx, payload)
        await self._write(packet)

        try:
            resp = await asyncio.wait_for(self._rx_queue.get(), timeout)
        except asyncio.TimeoutError:
            raise TimeoutError(f"No response from 0x{rx:03X} within {timeout}s")

        return resp["payload"]

    async def uds_read_did(self, tx: int, rx: int, did: int,
                            timeout: float = 5.0) -> bytes:
        """ReadDataByIdentifier (0x22) — returns value bytes only."""
        payload = struct.pack(">BH", 0x22, did)
        resp = await self.uds_request(tx, rx, payload, timeout)
        if resp[0] == 0x62:
            return resp[3:]   # skip 0x62, DID high, DID low
        if resp[0] == 0x7F:
            nrc = resp[2] if len(resp) > 2 else 0
            raise RuntimeError(f"NRC 0x{nrc:02X} on DID 0x{did:04X}")
        return resp

    async def uds_write_did(self, tx: int, rx: int, did: int,
                             value: bytes, timeout: float = 5.0) -> bytes:
        """WriteDataByIdentifier (0x2E)."""
        payload = struct.pack(">BH", 0x2E, did) + value
        return await self.uds_request(tx, rx, payload, timeout)

    async def uds_session(self, tx: int, rx: int,
                           session: int = 0x03, timeout: float = 5.0) -> bytes:
        """DiagnosticSessionControl (0x10)."""
        return await self.uds_request(tx, rx, bytes([0x10, session]), timeout)

    async def uds_read_memory(self, tx: int, rx: int,
                               address: int, length: int,
                               timeout: float = 5.0) -> bytes:
        """ReadMemoryByAddress (0x23) — 4-byte address, 2-byte length."""
        payload = bytes([0x23, 0x14]) + struct.pack(">IH", address, length)
        return await self.uds_request(tx, rx, payload, timeout)

    # ── Settings ──────────────────────────────────────────────────────────────
    async def _send_setting(self, setting_id: int, value: bytes):
        packet = build_packet(0x0000, 0x0000, value,
                               flags=FLAG_SETTINGS | FLAG_SETTINGS_GET)
        # Prepend the setting ID as first payload byte using a different packing
        # The bridge expects: [hdID][cmdFlags | setting_id encoding]...
        # Looking at isotp_bridge.c parse_packet: it checks header->cmdFlags & BRG_SETTING_*
        # and setting ID is embedded in cmdFlags when FLAG_SETTINGS is set.
        # Reconstruct: flags = FLAG_SETTINGS | FLAG_SETTINGS_GET | setting_id
        flags = FLAG_SETTINGS | FLAG_SETTINGS_GET | setting_id
        packet = build_packet(0x0000, 0x0000, value, flags=flags)
        await self._write(packet)

    async def set_raw_sniff(self, enabled: bool):
        """Enable or disable raw CAN frame sniffing."""
        await self._send_setting(SETTING_RAW_SNIFF, bytes([1 if enabled else 0]))
        log.info("Raw sniff: %s", "ON" if enabled else "OFF")

    async def set_raw_sniff_callback(self, cb: Optional[Callable]):
        """Register a callback for raw frames: cb(RawFrame)."""
        self._raw_sniff_cb = cb

    # ── Persist / poll mode ───────────────────────────────────────────────────
    async def persist_add(self, tx: int, rx: int, payload: bytes):
        """Add a UDS request to the persist poll list."""
        packet = build_packet(tx, rx, payload,
                               flags=FLAG_PER_ADD)
        await self._write(packet)

    async def persist_start(self):
        """Start the persist polling loop."""
        packet = build_packet(0, 0, b"", flags=FLAG_PER_ENABLE)
        await self._write(packet)

    async def persist_stop(self):
        """Stop persist mode and clear stored messages."""
        packet = build_packet(0, 0, b"", flags=FLAG_PER_CLEAR)
        await self._write(packet)

    # ── Raw sniff async iterator ──────────────────────────────────────────────
    async def sniff_frames(self, timeout: float = 30.0) -> AsyncIterator[RawFrame]:
        """Async generator yielding raw CAN frames while sniff mode is active."""
        deadline = asyncio.get_event_loop().time() + timeout
        while True:
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                break
            try:
                frame = await asyncio.wait_for(
                    self._sniff_queue.get(), timeout=min(remaining, 1.0))
                yield frame
            except asyncio.TimeoutError:
                continue

    # ── Convenience: read all standard ECU info DIDs ─────────────────────────
    async def get_ecu_info(self, tx: int, rx: int) -> dict:
        """Read the standard VW/Simos identification DIDs from a module."""
        info_dids = {
            0xF190: "VIN",
            0xF18C: "ECU Serial",
            0xF187: "Part Number",
            0xF189: "SW Version",
            0xF191: "HW Number",
            0xF197: "System Name",
            0xF1AD: "Engine Code",
            0xF17C: "FAZIT",
            0xF19E: "ASAM File ID",
        }
        await self.uds_session(tx, rx, session=0x03)

        result = {}
        for did, label in info_dids.items():
            try:
                raw = await self.uds_read_did(tx, rx, did, timeout=2.0)
                try:
                    result[label] = raw.decode("ascii").strip("\x00")
                except Exception:
                    result[label] = raw.hex()
            except Exception as e:
                result[label] = f"<error: {e}>"
        return result

    # ── Scan a DID range on a module ─────────────────────────────────────────
    async def scan_dids(self, tx: int, rx: int,
                         start: int = 0x0000, end: int = 0x0200,
                         progress_cb: Optional[Callable] = None) -> dict:
        """
        Brute-force scan a DID range.  Returns {did: bytes} for all responsive DIDs.
        Useful for finding undocumented constellation DIDs on J533.
        """
        await self.uds_session(tx, rx, session=0x03)
        found = {}
        total = end - start
        for i, did in enumerate(range(start, end)):
            if progress_cb:
                progress_cb(i, total, did)
            try:
                raw = await self.uds_read_did(tx, rx, did, timeout=0.5)
                found[did] = raw
                log.info("DID 0x%04X: %s", did, raw.hex())
            except TimeoutError:
                pass
            except RuntimeError as e:
                # NRC 0x31 = requestOutOfRange (DID doesn't exist) → skip silently
                if "0x31" not in str(e):
                    log.debug("DID 0x%04X: %s", did, e)
        return found
