"""Minimal client for PPSSPP's WebSocket debugger (stdlib only).

Start the emulator with   ppsspp-headless GAME.iso --debugger=PORT [--graphics=software]
(it breaks at the first instruction), then:

    from ppsspp_dbg import Debugger
    with Debugger(PORT) as dbg:
        dbg.call("cpu.resume")
        data = dbg.read(0x08804000, 0x100)

Protocol: JSON text frames on ws://127.0.0.1:PORT/debugger, subprotocol
"debugger.ppsspp.org". Each request is {"event": name, ...}; the reply carries the
same "event" (or "error"). Unsolicited events (e.g. "cpu.stepping", "log") arrive in
between and are queued; wait_event() returns them.
"""
import base64
import json
import os
import socket
import struct
import time


class Debugger:
    def __init__(self, port, host="127.0.0.1", connect_timeout=30.0):
        deadline = time.time() + connect_timeout
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET /debugger HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               "Sec-WebSocket-Protocol: debugger.ppsspp.org\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed during handshake")
            head += chunk
        status, _, rest = head.partition(b"\r\n\r\n")
        if b" 101 " not in status.split(b"\r\n")[0]:
            raise ConnectionError(status.decode(errors="replace"))
        self.buf = rest
        self.queue = []
        self.sock.settimeout(None)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # --- framing -----------------------------------------------------------
    def _send_text(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            hdr = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(hdr + mask + masked)

    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise ConnectionError("debugger connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _recv_message(self):
        data = b""
        while True:
            b0, b1 = self._recv_exact(2)
            n = b1 & 0x7f
            if n == 126:
                (n,) = struct.unpack("!H", self._recv_exact(2))
            elif n == 127:
                (n,) = struct.unpack("!Q", self._recv_exact(8))
            mask = self._recv_exact(4) if b1 & 0x80 else None
            payload = self._recv_exact(n)
            if mask:
                payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
            op = b0 & 0x0f
            if op == 0x8:
                raise ConnectionError("debugger closed the connection")
            if op == 0x9:                        # ping -> pong
                self.sock.sendall(struct.pack("!BB", 0x8a, 0x80) + os.urandom(4))
                continue
            data += payload
            if b0 & 0x80:
                return json.loads(data)

    # --- API -----------------------------------------------------------------
    def call(self, event, timeout=30.0, **params):
        """Send a request and return its reply; other events are queued."""
        ticket = os.urandom(4).hex()
        self._send_text(json.dumps({"event": event, "ticket": ticket, **params}))
        self.sock.settimeout(timeout)
        try:
            while True:
                msg = self._recv_message()
                # Most replies echo the ticket; cpu.stepping / cpu.resume answer with
                # an untagged broadcast of the same name instead.
                untagged = "ticket" not in msg and msg.get("event") in (event, "error")
                if msg.get("ticket") == ticket or untagged:
                    if msg.get("event") == "error":
                        raise RuntimeError(f"{event}: {msg.get('message')}")
                    return msg
                self.queue.append(msg)
        finally:
            self.sock.settimeout(None)

    def wait_event(self, name, timeout=60.0):
        for i, msg in enumerate(self.queue):
            if msg.get("event") == name:
                return self.queue.pop(i)
        self.sock.settimeout(timeout)
        try:
            while True:
                msg = self._recv_message()
                if msg.get("event") == name:
                    return msg
                self.queue.append(msg)
        finally:
            self.sock.settimeout(None)

    def read(self, address, size):
        out = b""
        while size > 0:                            # keep replies small
            n = min(size, 0x10000)
            out += base64.b64decode(self.call("memory.read", address=address, size=n)["base64"])
            address += n
            size -= n
        return out

    def u32(self, address):
        return struct.unpack("<I", self.read(address, 4))[0]

    def pause(self):
        if not self.call("cpu.status").get("stepping"):
            self.call("cpu.stepping")

    def resume(self):
        if self.call("cpu.status").get("stepping"):
            self.call("cpu.resume")

    def screenshot(self, path):
        """Pause, save the displayed frame as PNG, resume (screenshots need stepping)."""
        was = self.call("cpu.status").get("stepping")
        self.pause()
        try:
            msg = self.call("gpu.buffer.screenshot", type="base64")
        finally:
            if not was:
                self.call("cpu.resume")
        with open(path, "wb") as f:
            f.write(base64.b64decode(msg["base64"]))
        return msg

    def press(self, button, frames=6):
        """Hold a button (cross, circle, square, triangle, up, down, left, right,
        start, select, ltrigger, rtrigger) for some frames, then release."""
        self.call("input.buttons.press", button=button, duration=frames)
