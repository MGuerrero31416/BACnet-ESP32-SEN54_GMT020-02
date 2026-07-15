#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Dual-COM BACnet MS/TP correlator for Poll-For-Master troubleshooting.

How to run:
1) Install dependency: pip install pyserial
2) Start capture:
   python dual_mstp_correlator.py --port-esp32 COM5 --port-sniffer COM6
3) Optional args:
   --baud-esp32 115200 --baud-sniffer 38400 --window-ms 200 --out mstp_dual_capture.jsonl

This script is receive-only. It never writes to either serial port.
"""

from __future__ import annotations

import argparse
import json
import queue
import re
import signal
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Deque, Dict, List, Optional, Tuple

import serial
from serial import SerialException

FRAME_NAMES: Dict[int, str] = {
    0: "Token",
    1: "Poll For Master",
    2: "Reply To PFM",
    3: "Test Request",
    4: "Test Response",
    5: "BACnet Data Expecting Reply",
    6: "BACnet Data Not Expecting Reply",
    7: "Reply Postponed",
}

RE_PFM_RX = re.compile(r"PFM_RX src=(\d+) dst=(\d+) t=(\d+)")
RE_PFM_REPLY_TX = re.compile(
    r"PFM_REPLY_TX src=(\d+) dst=(\d+)(?: state=(\S+))? t_before=(\d+) t_after=(\d+) result=(\S+)"
)
RE_PFM_REPLY_SKIPPED = re.compile(r"PFM_REPLY_SKIPPED reason=(\S+) t=(\d+)")
RE_TOKEN_RX = re.compile(r"TOKEN_RX src=(\d+) dst=(\d+) t=(\d+)")
RE_TOKEN_NEXT = re.compile(r"TOKEN_NEXT our=(\d+) next=(\d+) t=(\d+)")
RE_TOKEN_PASS_TX = re.compile(
    r"TOKEN_PASS_TX src=(\d+) dst=(\d+) t_before=(\d+) t_after=(\d+) result=(\S+)"
)
RE_TOKEN_PASS_SKIPPED = re.compile(r"TOKEN_PASS_SKIPPED reason=(\S+) t=(\d+)")

RAW_CAPTURE_PRE_S = 0.050
RAW_CAPTURE_POST_S = 0.100
RAW_CAPTURE_RETENTION_S = 2.000
FRAME_NEIGHBORHOOD_S = 0.500
CONTEXT_BEFORE_BYTES = 16
CONTEXT_AFTER_BYTES = 16

FRAME_TYPE_POLL_FOR_MASTER = 1
FRAME_TYPE_REPLY_TO_PFM = 2
FRAME_TYPE_TOKEN = 0
FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY = 5
FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY = 6
FRAME_TYPE_BACNET_EXTENDED_DATA_EXPECTING_REPLY = 32
FRAME_TYPE_BACNET_EXTENDED_DATA_NOT_EXPECTING_REPLY = 33


class JsonlWriter:
    def __init__(self, path: str) -> None:
        self.path = path
        self._lock = threading.Lock()
        self._fh = open(path, "a", encoding="utf-8")

    def write(self, obj: Dict[str, Any]) -> None:
        line = json.dumps(obj, ensure_ascii=True, separators=(",", ":"))
        with self._lock:
            self._fh.write(line + "\n")
            self._fh.flush()

    def close(self) -> None:
        with self._lock:
            try:
                self._fh.flush()
            finally:
                self._fh.close()


class MstpFrameParser:
    """Incremental parser for raw BACnet MS/TP byte stream."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes) -> List[Tuple[int, int, int, int, bytes]]:
        """
        Returns list of tuples:
        (frame_type, dst, src, data_len, raw_frame)
        """
        frames: List[Tuple[int, int, int, int, bytes]] = []
        if not data:
            return frames

        self.buf.extend(data)

        while True:
            idx = self.buf.find(b"\x55\xFF")
            if idx < 0:
                # keep at most one trailing 0x55 to allow split preamble recovery
                if len(self.buf) > 1:
                    if self.buf[-1] == 0x55:
                        self.buf[:] = b"\x55"
                    else:
                        self.buf.clear()
                break

            if idx > 0:
                del self.buf[:idx]

            if len(self.buf) < 8:
                break

            frame_type = self.buf[2]
            dst = self.buf[3]
            src = self.buf[4]
            data_len = (self.buf[5] << 8) | self.buf[6]

            total_len = 8 + data_len + (2 if data_len > 0 else 0)
            if len(self.buf) < total_len:
                break

            raw = bytes(self.buf[:total_len])
            del self.buf[:total_len]
            frames.append((frame_type, dst, src, data_len, raw))

        return frames


@dataclass
class CycleContext:
    cycle_id: int
    trigger_ts: float
    deadline_ts: float
    rs485_pfm_event: Dict[str, Any]

    esp32_rx: Optional[Dict[str, Any]] = None
    esp32_reply_tx: Optional[Dict[str, Any]] = None
    esp32_skipped: Optional[Dict[str, Any]] = None
    rs485_reply: Optional[Dict[str, Any]] = None
    next_token: Optional[Dict[str, Any]] = None


@dataclass
class RawChunk:
    host_ts: float
    data: bytes


@dataclass
class RawCaptureContext:
    capture_id: int
    trigger_event: Dict[str, Any]
    trigger_ts: float
    start_ts: float
    end_ts: float
    chunks: List[RawChunk] = field(default_factory=list)


@dataclass
class WatchConfig:
    predecessor_mac: int
    our_mac: int
    fallback_next_mac: int


@dataclass
class RawReplyPatternConfig:
    expected_full: bytes
    preamble_type: bytes
    header_body: bytes
    tail: bytes


@dataclass
class PostOkContext:
    post_ok_id: int
    cycle_id: int
    trigger_ts: float
    start_ts: float
    end_ts: float
    token_pred_our_seen: bool = False
    src_our_frames: int = 0
    token_our_fallback_seen: bool = False
    pfm_from_our_seen: bool = False
    bacnet_data_from_our_seen: bool = False
    token_fallback_to5_after_our_seen: bool = False
    first_our_activity_ts: Optional[float] = None
    next_after_our: Optional[Dict[str, Any]] = None
    esp32_token_rx_seen: bool = False
    esp32_token_next_mac: Optional[int] = None
    esp32_token_pass_tx_seen: bool = False
    esp32_token_pass_dst: Optional[int] = None
    rs485_token_33_0_seen: bool = False


@dataclass
class CorrelatorState:
    window_s: float
    start_mono: float
    writer: JsonlWriter
    watch: WatchConfig
    raw_pattern: RawReplyPatternConfig

    recent_events: Deque[Dict[str, Any]] = field(default_factory=deque)
    cycles: List[CycleContext] = field(default_factory=list)
    next_cycle_id: int = 1
    sniffer_raw_chunks: Deque[RawChunk] = field(default_factory=deque)
    raw_captures: List[RawCaptureContext] = field(default_factory=list)
    next_capture_id: int = 1
    post_ok_window_s: float = 2.0
    post_ok_contexts: List[PostOkContext] = field(default_factory=list)
    next_post_ok_id: int = 1

    def prune_old_events(self, now: float) -> None:
        keep_after = now - max(5.0, self.window_s * 4.0)
        while self.recent_events and self.recent_events[0]["host_ts"] < keep_after:
            self.recent_events.popleft()
        raw_keep_after = now - RAW_CAPTURE_RETENTION_S
        while self.sniffer_raw_chunks and self.sniffer_raw_chunks[0].host_ts < raw_keep_after:
            self.sniffer_raw_chunks.popleft()


def now_host() -> float:
    return time.time()


def make_event(stream: str, kind: str, payload: Dict[str, Any], host_ts: Optional[float] = None) -> Dict[str, Any]:
    ts = now_host() if host_ts is None else host_ts
    evt: Dict[str, Any] = {
        "host_ts": ts,
        "stream": stream,
        "kind": kind,
    }
    evt.update(payload)
    return evt


def bytes_to_hex_spaced(data: bytes) -> str:
    if not data:
        return ""
    return " ".join(f"{b:02X}" for b in data)


def mstp_crc_calc_header(data_value: int, crc_value: int) -> int:
    crc = (crc_value ^ data_value) & 0xFFFF
    crc = crc ^ (crc << 1) ^ (crc << 2) ^ (crc << 3) ^ (crc << 4) ^ (crc << 5) ^ (crc << 6) ^ (crc << 7)
    return ((crc & 0xFE) ^ ((crc >> 8) & 1)) & 0xFF


def build_reply_to_pfm_pattern(predecessor_mac: int, our_mac: int) -> RawReplyPatternConfig:
    frame_type = FRAME_TYPE_REPLY_TO_PFM
    dst = predecessor_mac & 0xFF
    src = our_mac & 0xFF
    len_hi = 0x00
    len_lo = 0x00

    crc = 0xFF
    for b in (frame_type, dst, src, len_hi, len_lo):
        crc = mstp_crc_calc_header(b, crc)
    header_crc = (~crc) & 0xFF

    expected_full = bytes([0x55, 0xFF, frame_type, dst, src, len_hi, len_lo, header_crc])
    preamble_type = bytes([0x55, 0xFF, frame_type])
    header_body = bytes([frame_type, dst, src, len_hi, len_lo])
    tail = bytes([dst, src, len_hi, len_lo, header_crc])

    return RawReplyPatternConfig(
        expected_full=expected_full,
        preamble_type=preamble_type,
        header_body=header_body,
        tail=tail,
    )


def find_pattern_with_context(data: bytes, pattern: bytes, before: int, after: int) -> Optional[Dict[str, Any]]:
    idx = data.find(pattern)
    if idx < 0:
        return None
    start = max(0, idx - before)
    end = min(len(data), idx + len(pattern) + after)
    context = data[start:end]
    return {
        "offset": idx,
        "pattern_hex": bytes_to_hex_spaced(pattern),
        "context_start": start,
        "context_end": end,
        "context_hex": bytes_to_hex_spaced(context),
    }


def frame_summary(evt: Dict[str, Any]) -> str:
    return f"{evt.get('frame_name')} {evt.get('src')}->{evt.get('dst')}"


def serial_reader_esp32(
    port: str,
    baud: int,
    out_q: queue.Queue,
    stop_event: threading.Event,
    retry_delay_s: float = 1.0,
) -> None:
    while not stop_event.is_set():
        ser: Optional[serial.Serial] = None
        try:
            ser = serial.Serial(port=port, baudrate=baud, timeout=0.2)
            print(f"[esp32] opened {port} @ {baud}")
            while not stop_event.is_set():
                try:
                    line = ser.readline()
                except (SerialException, OSError) as e:
                    print(f"[esp32] read error: {e}; reopening in {retry_delay_s:.1f}s")
                    break

                if not line:
                    continue

                host_ts = now_host()
                text = line.decode("utf-8", errors="replace").rstrip("\r\n")
                out_q.put(make_event("esp32", "raw_line", {"line": text}, host_ts=host_ts))

                m = RE_PFM_RX.search(text)
                if m:
                    src, dst, t_dev = m.groups()
                    out_q.put(
                        make_event(
                            "esp32",
                            "pfm_rx",
                            {
                                "src": int(src),
                                "dst": int(dst),
                                "t_dev_us": int(t_dev),
                            },
                            host_ts=host_ts,
                        )
                    )
                    continue

                m = RE_PFM_REPLY_TX.search(text)
                if m:
                    src, dst, state_name, t_before, t_after, result = m.groups()
                    payload = {
                        "src": int(src),
                        "dst": int(dst),
                        "t_before_dev_us": int(t_before),
                        "t_after_dev_us": int(t_after),
                        "result": result,
                    }
                    if state_name is not None:
                        payload["state"] = state_name
                    out_q.put(
                        make_event(
                            "esp32",
                            "pfm_reply_tx",
                            payload,
                            host_ts=host_ts,
                        )
                    )
                    continue

                m = RE_PFM_REPLY_SKIPPED.search(text)
                if m:
                    reason, t_dev = m.groups()
                    out_q.put(
                        make_event(
                            "esp32",
                            "pfm_reply_skipped",
                            {
                                "reason": reason,
                                "t_dev_us": int(t_dev),
                            },
                            host_ts=host_ts,
                        )
                    )

                m = RE_TOKEN_RX.search(text)
                if m:
                    src, dst, t_dev = m.groups()
                    out_q.put(
                        make_event(
                            "esp32",
                            "token_rx",
                            {
                                "src": int(src),
                                "dst": int(dst),
                                "t_dev_us": int(t_dev),
                            },
                            host_ts=host_ts,
                        )
                    )
                    continue

                m = RE_TOKEN_NEXT.search(text)
                if m:
                    our_mac, next_mac, t_dev = m.groups()
                    out_q.put(
                        make_event(
                            "esp32",
                            "token_next",
                            {
                                "our": int(our_mac),
                                "next": int(next_mac),
                                "t_dev_us": int(t_dev),
                            },
                            host_ts=host_ts,
                        )
                    )
                    continue

                m = RE_TOKEN_PASS_TX.search(text)
                if m:
                    src, dst, t_before, t_after, result = m.groups()
                    out_q.put(
                        make_event(
                            "esp32",
                            "token_pass_tx",
                            {
                                "src": int(src),
                                "dst": int(dst),
                                "t_before_dev_us": int(t_before),
                                "t_after_dev_us": int(t_after),
                                "result": result,
                            },
                            host_ts=host_ts,
                        )
                    )
                    continue

                m = RE_TOKEN_PASS_SKIPPED.search(text)
                if m:
                    reason, t_dev = m.groups()
                    out_q.put(
                        make_event(
                            "esp32",
                            "token_pass_skipped",
                            {
                                "reason": reason,
                                "t_dev_us": int(t_dev),
                            },
                            host_ts=host_ts,
                        )
                    )

        except (SerialException, OSError) as e:
            print(f"[esp32] open error: {e}; retrying in {retry_delay_s:.1f}s")
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass

        stop_event.wait(retry_delay_s)


def serial_reader_sniffer(
    port: str,
    baud: int,
    out_q: queue.Queue,
    stop_event: threading.Event,
    retry_delay_s: float = 1.0,
) -> None:
    parser = MstpFrameParser()

    while not stop_event.is_set():
        ser: Optional[serial.Serial] = None
        try:
            ser = serial.Serial(port=port, baudrate=baud, timeout=0.2)
            print(f"[sniffer] opened {port} @ {baud}")
            while not stop_event.is_set():
                try:
                    data = ser.read(512)
                except (SerialException, OSError) as e:
                    print(f"[sniffer] read error: {e}; reopening in {retry_delay_s:.1f}s")
                    break

                if not data:
                    continue

                host_ts = now_host()
                out_q.put(
                    make_event(
                        "sniffer",
                        "sniffer_raw_chunk",
                        {
                            "raw_bytes": data,
                        },
                        host_ts=host_ts,
                    )
                )
                for frame_type, dst, src, data_len, raw in parser.feed(data):
                    out_q.put(
                        make_event(
                            "sniffer",
                            "mstp_frame",
                            {
                                "frame_type": frame_type,
                                "frame_name": FRAME_NAMES.get(frame_type, f"Unknown {frame_type}"),
                                "dst": dst,
                                "src": src,
                                "data_len": data_len,
                                "raw_hex": raw.hex().upper(),
                            },
                            host_ts=host_ts,
                        )
                    )

        except (SerialException, OSError) as e:
            print(f"[sniffer] open error: {e}; retrying in {retry_delay_s:.1f}s")
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass

        stop_event.wait(retry_delay_s)


def within_window(event_ts: float, center_ts: float, window_s: float) -> bool:
    return (center_ts - window_s) <= event_ts <= (center_ts + window_s)


def event_token_summary(evt: Optional[Dict[str, Any]]) -> str:
    if not evt:
        return "none"
    return f"{evt.get('src')}->{evt.get('dst')}"


def classify_cycle(cycle: CycleContext, watch: WatchConfig) -> str:
    if cycle.esp32_skipped is not None:
        return "SKIPPED"

    if cycle.esp32_rx is None:
        return "CASE_A"

    if cycle.esp32_rx is not None and cycle.esp32_reply_tx is not None and cycle.rs485_reply is None:
        return "CASE_B"

    if cycle.rs485_reply is not None:
        if cycle.next_token is not None:
            if cycle.next_token.get("dst") == watch.our_mac:
                return "OK"
            if cycle.next_token.get("dst") == watch.fallback_next_mac:
                return "CASE_C"
            return "CASE_C"

    return "UNRESOLVED"


def print_classification_line(cycle: CycleContext, verdict: str, start_mono: float, watch: WatchConfig) -> None:
    elapsed = time.monotonic() - start_mono
    esp_rx = "yes" if cycle.esp32_rx else "no"
    esp_reply = "yes" if cycle.esp32_reply_tx else "no"
    rs_reply = "yes" if cycle.rs485_reply else "no"
    next_token = event_token_summary(cycle.next_token)
    print(
        f"{elapsed:0.3f}s {verdict} PFM {watch.predecessor_mac}->{watch.our_mac} | "
        f"ESP32_RX={esp_rx} ESP32_REPLY={esp_reply} RS485_REPLY={rs_reply} NEXT_TOKEN={next_token}"
    )


def attach_event_to_cycle(cycle: CycleContext, evt: Dict[str, Any], window_s: float, watch: WatchConfig) -> None:
    ts = evt["host_ts"]
    t0 = cycle.trigger_ts

    if not within_window(ts, t0, window_s):
        return

    kind = evt.get("kind")

    if kind == "pfm_reply_skipped":
        if cycle.esp32_skipped is None:
            cycle.esp32_skipped = evt
        return

    if (
        kind == "pfm_rx"
        and evt.get("src") == watch.predecessor_mac
        and evt.get("dst") == watch.our_mac
    ):
        if cycle.esp32_rx is None:
            cycle.esp32_rx = evt
        return

    if (
        kind == "pfm_reply_tx"
        and evt.get("src") == watch.our_mac
        and evt.get("dst") == watch.predecessor_mac
    ):
        if cycle.esp32_reply_tx is None:
            cycle.esp32_reply_tx = evt
        return

    if kind == "mstp_frame":
        frame_type = evt.get("frame_type")
        src = evt.get("src")
        dst = evt.get("dst")

        if frame_type == FRAME_TYPE_REPLY_TO_PFM and src == watch.our_mac and dst == watch.predecessor_mac:
            if cycle.rs485_reply is None:
                cycle.rs485_reply = evt
            return

        if frame_type == FRAME_TYPE_TOKEN and src == watch.predecessor_mac:
            if ts >= t0 and cycle.next_token is None:
                cycle.next_token = evt
            return


def seed_cycle_from_recent(
    cycle: CycleContext,
    recent_events: Deque[Dict[str, Any]],
    window_s: float,
    watch: WatchConfig,
) -> None:
    for evt in recent_events:
        attach_event_to_cycle(cycle, evt, window_s, watch)


def attach_chunk_to_capture(capture: RawCaptureContext, chunk: RawChunk) -> None:
    if capture.start_ts <= chunk.host_ts <= capture.end_ts:
        capture.chunks.append(chunk)


def seed_capture_from_recent(capture: RawCaptureContext, chunks: Deque[RawChunk]) -> None:
    for chunk in chunks:
        attach_chunk_to_capture(capture, chunk)


def attach_event_to_post_ok(ctx: PostOkContext, evt: Dict[str, Any], watch: WatchConfig) -> None:
    ts = float(evt.get("host_ts", 0.0))
    if ts < ctx.start_ts or ts > ctx.end_ts:
        return

    kind = evt.get("kind")

    if kind == "token_rx":
        if evt.get("dst") == watch.our_mac:
            ctx.esp32_token_rx_seen = True
        return

    if kind == "token_next":
        if evt.get("our") == watch.our_mac and ctx.esp32_token_next_mac is None:
            ctx.esp32_token_next_mac = int(evt.get("next"))
        return

    if kind == "token_pass_tx":
        if evt.get("src") == watch.our_mac:
            ctx.esp32_token_pass_tx_seen = True
            if ctx.esp32_token_pass_dst is None:
                ctx.esp32_token_pass_dst = int(evt.get("dst"))
        return

    if kind != "mstp_frame":
        return

    frame_type = evt.get("frame_type")
    src = evt.get("src")
    dst = evt.get("dst")

    if frame_type == FRAME_TYPE_TOKEN and src == watch.predecessor_mac and dst == watch.our_mac:
        ctx.token_pred_our_seen = True

    if frame_type == FRAME_TYPE_TOKEN and src == 33 and dst == 0:
        ctx.rs485_token_33_0_seen = True

    if src == watch.our_mac:
        ctx.src_our_frames += 1
        if ctx.first_our_activity_ts is None:
            ctx.first_our_activity_ts = ts
        if frame_type == FRAME_TYPE_TOKEN and dst == watch.fallback_next_mac:
            ctx.token_our_fallback_seen = True
        if frame_type == FRAME_TYPE_POLL_FOR_MASTER:
            ctx.pfm_from_our_seen = True
        if frame_type in (
            FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY,
            FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY,
            FRAME_TYPE_BACNET_EXTENDED_DATA_EXPECTING_REPLY,
            FRAME_TYPE_BACNET_EXTENDED_DATA_NOT_EXPECTING_REPLY,
        ):
            ctx.bacnet_data_from_our_seen = True

    if (
        ctx.first_our_activity_ts is not None
        and ts >= ctx.first_our_activity_ts
        and frame_type == FRAME_TYPE_TOKEN
        and src == watch.fallback_next_mac
        and dst == 5
    ):
        ctx.token_fallback_to5_after_our_seen = True

    if (
        ctx.first_our_activity_ts is not None
        and ts > ctx.first_our_activity_ts
        and ctx.next_after_our is None
    ):
        ctx.next_after_our = {
            "host_ts": ts,
            "frame_type": frame_type,
            "frame_name": evt.get("frame_name"),
            "src": src,
            "dst": dst,
            "data_len": evt.get("data_len"),
        }


def seed_post_ok_from_recent(ctx: PostOkContext, recent_events: Deque[Dict[str, Any]], watch: WatchConfig) -> None:
    for evt in recent_events:
        attach_event_to_post_ok(ctx, evt, watch)


def finalize_due_post_ok_contexts(state: CorrelatorState, now: float) -> None:
    keep: List[PostOkContext] = []
    for ctx in state.post_ok_contexts:
        if now < ctx.end_ts:
            keep.append(ctx)
            continue

        next_after_summary = (
            f"{ctx.next_after_our.get('frame_name')} {ctx.next_after_our.get('src')}->{ctx.next_after_our.get('dst')}"
            if ctx.next_after_our
            else "none"
        )

        elapsed = time.monotonic() - state.start_mono
        print(
            f"{elapsed:0.3f}s POST_OK "
            f"token_{state.watch.predecessor_mac}_{state.watch.our_mac}={'yes' if ctx.token_pred_our_seen else 'no'} "
            f"src{state.watch.our_mac}_frames={ctx.src_our_frames} "
            f"token_{state.watch.our_mac}_{state.watch.fallback_next_mac}={'yes' if ctx.token_our_fallback_seen else 'no'} "
            f"next_after_{state.watch.our_mac}={next_after_summary} "
            f"pfm_{state.watch.our_mac}_any={'yes' if ctx.pfm_from_our_seen else 'no'} "
            f"bacnet_data_{state.watch.our_mac}={'yes' if ctx.bacnet_data_from_our_seen else 'no'} "
            f"token_{state.watch.fallback_next_mac}_5_after_{state.watch.our_mac}={'yes' if ctx.token_fallback_to5_after_our_seen else 'no'} "
            f"ESP32_TOKEN_RX={'yes' if ctx.esp32_token_rx_seen else 'no'} "
            f"ESP32_TOKEN_NEXT={ctx.esp32_token_next_mac if ctx.esp32_token_next_mac is not None else 'none'} "
            f"ESP32_TOKEN_PASS_TX={'yes' if ctx.esp32_token_pass_tx_seen else 'no'} "
            f"ESP32_TOKEN_PASS_DST={ctx.esp32_token_pass_dst if ctx.esp32_token_pass_dst is not None else 'none'} "
            f"RS485_TOKEN_33_0={'yes' if ctx.rs485_token_33_0_seen else 'no'}"
        )

        evt = {
            "host_ts": now,
            "stream": "correlator",
            "kind": "post_ok",
            "post_ok_id": ctx.post_ok_id,
            "cycle_id": ctx.cycle_id,
            "window_start_ts": ctx.start_ts,
            "window_end_ts": ctx.end_ts,
            "watch_predecessor_mac": state.watch.predecessor_mac,
            "watch_our_mac": state.watch.our_mac,
            "watch_fallback_next_mac": state.watch.fallback_next_mac,
            "token_pred_our_seen": ctx.token_pred_our_seen,
            "src_our_frames": ctx.src_our_frames,
            "token_our_fallback_seen": ctx.token_our_fallback_seen,
            "pfm_from_our_seen": ctx.pfm_from_our_seen,
            "bacnet_data_from_our_seen": ctx.bacnet_data_from_our_seen,
            "token_fallback_to5_after_our_seen": ctx.token_fallback_to5_after_our_seen,
            "first_our_activity_ts": ctx.first_our_activity_ts,
            "next_after_our": ctx.next_after_our,
            "esp32_token_rx_seen": ctx.esp32_token_rx_seen,
            "esp32_token_next_mac": ctx.esp32_token_next_mac,
            "esp32_token_pass_tx_seen": ctx.esp32_token_pass_tx_seen,
            "esp32_token_pass_dst": ctx.esp32_token_pass_dst,
            "rs485_token_33_0_seen": ctx.rs485_token_33_0_seen,
        }
        state.writer.write(evt)

    state.post_ok_contexts = keep


def finalize_due_raw_captures(state: CorrelatorState, now: float) -> None:
    keep: List[RawCaptureContext] = []

    for capture in state.raw_captures:
        if now < capture.end_ts:
            keep.append(capture)
            continue

        capture.chunks.sort(key=lambda c: c.host_ts)
        raw_bytes = b"".join(chunk.data for chunk in capture.chunks)
        raw_hex = bytes_to_hex_spaced(raw_bytes)

        exact_found = state.raw_pattern.expected_full in raw_bytes
        preamble_type_match = find_pattern_with_context(
            raw_bytes,
            state.raw_pattern.preamble_type,
            CONTEXT_BEFORE_BYTES,
            CONTEXT_AFTER_BYTES,
        )
        header_body_match = find_pattern_with_context(
            raw_bytes,
            state.raw_pattern.header_body,
            CONTEXT_BEFORE_BYTES,
            CONTEXT_AFTER_BYTES,
        )
        tail_match = find_pattern_with_context(
            raw_bytes,
            state.raw_pattern.tail,
            CONTEXT_BEFORE_BYTES,
            CONTEXT_AFTER_BYTES,
        )

        preamble_type_found = preamble_type_match is not None
        header_body_found = header_body_match is not None
        tail_found = tail_match is not None

        trig_ts = capture.trigger_ts
        frame_win_start = trig_ts - FRAME_NEIGHBORHOOD_S
        frame_win_end = trig_ts + FRAME_NEIGHBORHOOD_S
        decoded_frames_window: List[Dict[str, Any]] = []
        prev_frame: Optional[Dict[str, Any]] = None
        next_frame: Optional[Dict[str, Any]] = None

        for evt in state.recent_events:
            if evt.get("kind") != "mstp_frame":
                continue
            evt_ts = float(evt.get("host_ts", 0.0))
            if frame_win_start <= evt_ts <= frame_win_end:
                decoded_frames_window.append(
                    {
                        "host_ts": evt_ts,
                        "frame_type": evt.get("frame_type"),
                        "frame_name": evt.get("frame_name"),
                        "src": evt.get("src"),
                        "dst": evt.get("dst"),
                        "data_len": evt.get("data_len"),
                    }
                )
            if evt_ts <= trig_ts:
                if (prev_frame is None) or (evt_ts > float(prev_frame.get("host_ts", 0.0))):
                    prev_frame = evt
            if evt_ts >= trig_ts:
                if (next_frame is None) or (evt_ts < float(next_frame.get("host_ts", 0.0))):
                    next_frame = evt

        src_pred_count = sum(1 for f in decoded_frames_window if f.get("src") == state.watch.predecessor_mac)
        src_fallback_count = sum(1 for f in decoded_frames_window if f.get("src") == state.watch.fallback_next_mac)

        prev_summary = (
            f"{prev_frame.get('frame_name')} {prev_frame.get('src')}->{prev_frame.get('dst')}"
            if prev_frame
            else "none"
        )
        next_summary = (
            f"{next_frame.get('frame_name')} {next_frame.get('src')}->{next_frame.get('dst')}"
            if next_frame
            else "none"
        )

        frames_summary = ",".join(
            f"{f.get('frame_type')}:{f.get('src')}->{f.get('dst')}"
            for f in decoded_frames_window
        )

        elapsed = time.monotonic() - state.start_mono
        print(
            f"{elapsed:0.3f}s RAW_AROUND_PFM_REPLY "
            f"RAW_REPLY_EXACT_FOUND {'yes' if exact_found else 'no'} "
            f"RAW_REPLY_PREAMBLE_TYPE_FOUND {'yes' if preamble_type_found else 'no'} "
            f"RAW_REPLY_HEADER_BODY_FOUND {'yes' if header_body_found else 'no'} "
            f"RAW_REPLY_TAIL_FOUND {'yes' if tail_found else 'no'} "
            f"DECODED_FRAMES_+-500MS={len(decoded_frames_window)} "
            f"SRC{state.watch.predecessor_mac}={src_pred_count} "
            f"SRC{state.watch.fallback_next_mac}={src_fallback_count} "
            f"PREV={prev_summary} NEXT={next_summary} "
            f"frames=[{frames_summary}] "
            f"raw_hex={raw_hex if raw_hex else '<empty>'}"
        )

        if preamble_type_match is not None:
            print(
                "RAW_AROUND_PFM_REPLY_CONTEXT "
                f"kind=preamble_type offset={preamble_type_match['offset']} "
                f"context={preamble_type_match['context_hex']}"
            )
        if header_body_match is not None:
            print(
                "RAW_AROUND_PFM_REPLY_CONTEXT "
                f"kind=header_body offset={header_body_match['offset']} "
                f"context={header_body_match['context_hex']}"
            )
        if tail_match is not None:
            print(
                "RAW_AROUND_PFM_REPLY_CONTEXT "
                f"kind=tail offset={tail_match['offset']} "
                f"context={tail_match['context_hex']}"
            )

        raw_evt = {
            "host_ts": now,
            "stream": "correlator",
            "kind": "raw_around_pfm_reply",
            "label": "RAW_AROUND_PFM_REPLY",
            "capture_id": capture.capture_id,
            "trigger_event": capture.trigger_event,
            "window_start_ts": capture.start_ts,
            "window_end_ts": capture.end_ts,
            "raw_reply_exact_found": exact_found,
            "raw_reply_preamble_type_found": preamble_type_found,
            "raw_reply_header_body_found": header_body_found,
            "raw_reply_tail_found": tail_found,
            "raw_reply_preamble_type_match": preamble_type_match,
            "raw_reply_header_body_match": header_body_match,
            "raw_reply_tail_match": tail_match,
            "decoded_frame_window_s": FRAME_NEIGHBORHOOD_S,
            "decoded_frames_window_count": len(decoded_frames_window),
            "decoded_frames_src_predecessor_count": src_pred_count,
            "decoded_frames_src_fallback_count": src_fallback_count,
            "watch_predecessor_mac": state.watch.predecessor_mac,
            "watch_our_mac": state.watch.our_mac,
            "watch_fallback_next_mac": state.watch.fallback_next_mac,
            "decoded_previous_frame": {
                "host_ts": prev_frame.get("host_ts"),
                "frame_type": prev_frame.get("frame_type"),
                "frame_name": prev_frame.get("frame_name"),
                "src": prev_frame.get("src"),
                "dst": prev_frame.get("dst"),
                "data_len": prev_frame.get("data_len"),
            }
            if prev_frame
            else None,
            "decoded_next_frame": {
                "host_ts": next_frame.get("host_ts"),
                "frame_type": next_frame.get("frame_type"),
                "frame_name": next_frame.get("frame_name"),
                "src": next_frame.get("src"),
                "dst": next_frame.get("dst"),
                "data_len": next_frame.get("data_len"),
            }
            if next_frame
            else None,
            "decoded_frames_window": decoded_frames_window,
            "raw_len_bytes": len(raw_bytes),
            "raw_hex": raw_hex,
        }
        state.writer.write(raw_evt)

    state.raw_captures = keep


def finalize_due_cycles(state: CorrelatorState, now: float) -> None:
    keep: List[CycleContext] = []
    for cycle in state.cycles:
        if now < cycle.deadline_ts:
            keep.append(cycle)
            continue

        verdict = classify_cycle(cycle, state.watch)
        print_classification_line(cycle, verdict, state.start_mono, state.watch)

        class_evt = {
            "host_ts": now,
            "stream": "correlator",
            "kind": "classification",
            "cycle_id": cycle.cycle_id,
            "classification": verdict,
            "trigger": cycle.rs485_pfm_event,
            "esp32_rx": cycle.esp32_rx,
            "esp32_reply_tx": cycle.esp32_reply_tx,
            "esp32_skipped": cycle.esp32_skipped,
            "rs485_reply": cycle.rs485_reply,
            "next_token": cycle.next_token,
            "watch_predecessor_mac": state.watch.predecessor_mac,
            "watch_our_mac": state.watch.our_mac,
            "watch_fallback_next_mac": state.watch.fallback_next_mac,
        }
        state.writer.write(class_evt)

        if verdict == "OK" and cycle.next_token is not None:
            token_ts = float(cycle.next_token.get("host_ts", now))
            post_ctx = PostOkContext(
                post_ok_id=state.next_post_ok_id,
                cycle_id=cycle.cycle_id,
                trigger_ts=now,
                start_ts=token_ts,
                end_ts=token_ts + state.post_ok_window_s,
                token_pred_our_seen=True,
            )
            state.next_post_ok_id += 1
            seed_post_ok_from_recent(post_ctx, state.recent_events, state.watch)
            state.post_ok_contexts.append(post_ctx)

    state.cycles = keep


def process_event(state: CorrelatorState, evt: Dict[str, Any]) -> None:
    now = evt["host_ts"]

    if evt.get("kind") == "sniffer_raw_chunk":
        raw_bytes = evt.get("raw_bytes", b"")
        if isinstance(raw_bytes, (bytes, bytearray)) and raw_bytes:
            chunk = RawChunk(host_ts=now, data=bytes(raw_bytes))
            state.sniffer_raw_chunks.append(chunk)
            for capture in state.raw_captures:
                attach_chunk_to_capture(capture, chunk)
        state.prune_old_events(now)
        finalize_due_raw_captures(state, now)
        return

    state.writer.write(evt)

    state.recent_events.append(evt)
    state.prune_old_events(now)

    for post_ctx in state.post_ok_contexts:
        attach_event_to_post_ok(post_ctx, evt, state.watch)

    for cycle in state.cycles:
        attach_event_to_cycle(cycle, evt, state.window_s, state.watch)

    if (
        evt.get("kind") == "pfm_reply_tx"
        and evt.get("src") == state.watch.our_mac
        and evt.get("dst") == state.watch.predecessor_mac
    ):
        capture = RawCaptureContext(
            capture_id=state.next_capture_id,
            trigger_event=evt,
            trigger_ts=evt["host_ts"],
            start_ts=evt["host_ts"] - RAW_CAPTURE_PRE_S,
            end_ts=evt["host_ts"] + RAW_CAPTURE_POST_S,
        )
        state.next_capture_id += 1
        seed_capture_from_recent(capture, state.sniffer_raw_chunks)
        state.raw_captures.append(capture)

    if (
        evt.get("kind") == "mstp_frame"
        and evt.get("frame_type") == FRAME_TYPE_POLL_FOR_MASTER
        and evt.get("src") == state.watch.predecessor_mac
        and evt.get("dst") == state.watch.our_mac
    ):
        cycle = CycleContext(
            cycle_id=state.next_cycle_id,
            trigger_ts=evt["host_ts"],
            deadline_ts=evt["host_ts"] + state.window_s,
            rs485_pfm_event=evt,
        )
        state.next_cycle_id += 1
        seed_cycle_from_recent(cycle, state.recent_events, state.window_s, state.watch)
        state.cycles.append(cycle)

    finalize_due_cycles(state, now)
    finalize_due_raw_captures(state, now)
    finalize_due_post_ok_contexts(state, now)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Dual COM BACnet MS/TP capture and correlator")
    p.add_argument("--port-esp32", required=True, help="ESP32 serial log COM port")
    p.add_argument("--port-sniffer", required=True, help="USB-RS485 passive sniffer COM port")
    p.add_argument("--baud-esp32", type=int, default=115200, help="ESP32 baud rate (default: 115200)")
    p.add_argument("--baud-sniffer", type=int, default=38400, help="Sniffer baud rate (default: 38400)")
    p.add_argument("--window-ms", type=int, default=200, help="Correlation half-window in ms (default: 200)")
    p.add_argument(
        "--post-ok-window-ms",
        type=int,
        default=2000,
        help="Post-OK token lifecycle observation window in ms (default: 2000)",
    )
    p.add_argument(
        "--predecessor-mac",
        type=int,
        default=16,
        help="Predecessor MAC that sends Poll-For-Master (default: 16)",
    )
    p.add_argument(
        "--our-mac",
        type=int,
        default=17,
        help="Our MAC expected to receive Poll-For-Master and send Reply-To-PFM (default: 17)",
    )
    p.add_argument(
        "--fallback-next-mac",
        type=int,
        default=32,
        help="Fallback next token destination used in CASE_C reporting (default: 32)",
    )
    p.add_argument("--out", default="mstp_dual_capture.jsonl", help="JSONL output path")
    return p


def main() -> int:
    args = build_arg_parser().parse_args()

    window_s = max(1, args.window_ms) / 1000.0
    post_ok_window_s = max(1, args.post_ok_window_ms) / 1000.0

    for value, name in (
        (args.predecessor_mac, "predecessor-mac"),
        (args.our_mac, "our-mac"),
        (args.fallback_next_mac, "fallback-next-mac"),
    ):
        if value < 0 or value > 255:
            raise SystemExit(f"--{name} must be in range 0..255")

    watch = WatchConfig(
        predecessor_mac=args.predecessor_mac,
        our_mac=args.our_mac,
        fallback_next_mac=args.fallback_next_mac,
    )
    raw_pattern = build_reply_to_pfm_pattern(watch.predecessor_mac, watch.our_mac)

    out_q: "queue.Queue[Dict[str, Any]]" = queue.Queue(maxsize=10000)
    stop_event = threading.Event()
    writer = JsonlWriter(args.out)

    state = CorrelatorState(
        window_s=window_s,
        start_mono=time.monotonic(),
        writer=writer,
        watch=watch,
        raw_pattern=raw_pattern,
        post_ok_window_s=post_ok_window_s,
    )

    def handle_signal(_sig: int, _frame: Any) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    t_esp = threading.Thread(
        target=serial_reader_esp32,
        args=(args.port_esp32, args.baud_esp32, out_q, stop_event),
        daemon=True,
        name="reader-esp32",
    )
    t_snif = threading.Thread(
        target=serial_reader_sniffer,
        args=(args.port_sniffer, args.baud_sniffer, out_q, stop_event),
        daemon=True,
        name="reader-sniffer",
    )

    t_esp.start()
    t_snif.start()

    print(
        "capture started | "
        f"esp32={args.port_esp32}@{args.baud_esp32} "
        f"sniffer={args.port_sniffer}@{args.baud_sniffer} "
        f"window={args.window_ms}ms "
        f"post_ok_window={args.post_ok_window_ms}ms "
        f"watch={watch.predecessor_mac}->{watch.our_mac} "
        f"fallback_next={watch.fallback_next_mac} "
        f"expected_reply={bytes_to_hex_spaced(raw_pattern.expected_full)} "
        f"out={args.out}"
    )

    try:
        while not stop_event.is_set():
            try:
                evt = out_q.get(timeout=0.2)
                process_event(state, evt)
            except queue.Empty:
                finalize_due_cycles(state, now_host())
                finalize_due_raw_captures(state, now_host())
                finalize_due_post_ok_contexts(state, now_host())
    finally:
        stop_event.set()
        t_esp.join(timeout=1.0)
        t_snif.join(timeout=1.0)
        # Flush any remaining cycles as unresolved on shutdown.
        finalize_due_cycles(state, now_host() + state.window_s + 0.001)
        finalize_due_raw_captures(state, now_host() + RAW_CAPTURE_POST_S + 0.001)
        finalize_due_post_ok_contexts(state, now_host() + state.post_ok_window_s + 0.001)
        writer.close()
        print("capture stopped")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
