#!/usr/bin/env python3
"""Drive one device handoff on the CH340 and print the transcript.

Baud 115200 is the unconfirmed placeholder. This script opens only
/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0.
"""

import json
import os
import sys
import time

PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
BAUD = 115200  # unconfirmed placeholder

transcript = []


def fail(message):
    print("--- transcript ---")
    for line in transcript:
        print(line)
    print("--- end ---")
    print("RESULT", message)
    sys.exit(1)


def main():
    if not os.path.exists(PORT):
        fail("ch340-by-id-missing")
    real = os.path.realpath(PORT)
    if "ttyACM" in real:
        fail("refusing-acm-port")

    import serial

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 0.05
    ser.dsrdtr = False
    ser.rtscts = False
    ser.exclusive = True
    ser.open()
    # This CH340 holds the ESP32 in reset while RTS is high.
    ser.dtr = False
    ser.rts = False
    time.sleep(0.05)
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False

    buf = b""
    seq = 1

    def log(direction, text):
        transcript.append(f"{direction} {text}")

    def send(obj):
        line = json.dumps(obj, separators=(",", ":"))
        ser.write(line.encode("utf-8") + b"\n")
        ser.flush()
        log(">>", line)

    def nid(prefix):
        nonlocal seq
        value = f"d-{prefix}{seq}"
        seq += 1
        return value

    def drain(deadline):
        nonlocal buf
        found = []
        while time.time() < deadline:
            chunk = ser.read(512)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", errors="replace").rstrip("\r")
                if not text:
                    continue
                log("<<", text)
                if not text.startswith("{"):
                    continue
                try:
                    msg = json.loads(text)
                except json.JSONDecodeError:
                    continue
                if isinstance(msg, dict) and msg.get("type") == "ping":
                    send(
                        {
                            "v": 1,
                            "type": "pong",
                            "id": nid("pong"),
                            "payload": {"ref": msg.get("id", "")},
                        }
                    )
                    continue
                found.append(msg)
        return found

    def wait_type(type_name, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for msg in drain(min(time.time() + 0.2, deadline)):
                if msg.get("type") == type_name:
                    return msg
                if msg.get("type") == "handoff.abort":
                    fail(f"abort-while-waiting-for-{type_name}")
                if msg.get("type") == "error":
                    fail(f"error-while-waiting-for-{type_name}")
        fail(f"timeout-waiting-for-{type_name}")

    try:
        hello = wait_type("hello", 10)
        payload = hello.get("payload") or {}
        if payload.get("role") != "device" or payload.get("owner") != "device":
            fail("bad-device-hello")
        if "idle" not in (payload.get("expressions") or []):
            fail("hello-missing-idle")

        send(
            {
                "v": 1,
                "type": "hello.ack",
                "id": nid("hack"),
                "payload": {"ok": True, "proto": 1},
            }
        )
        send(
            {
                "v": 1,
                "type": "hello",
                "id": nid("hello"),
                "payload": {
                    "role": "desktop",
                    "proto": 1,
                    "expressions": ["idle"],
                    "owner": "device",
                },
            }
        )
        wait_type("hello.ack", 3)

        send(
            {
                "v": 1,
                "type": "handoff.request",
                "id": nid("req"),
                "payload": {"expression": "idle"},
            }
        )
        accept = wait_type("handoff.accept", 3)
        if (accept.get("payload") or {}).get("expression") != "idle":
            fail("accept-not-idle")

        send(
            {
                "v": 1,
                "type": "handoff.ready",
                "id": nid("ready"),
                "payload": {"expression": "idle"},
            }
        )
        commit = wait_type("handoff.commit", 3)
        commit_id = commit.get("id")
        if not commit_id:
            fail("commit-missing-id")
        send(
            {
                "v": 1,
                "type": "handoff.ack",
                "id": nid("cack"),
                "payload": {"ref": commit_id},
            }
        )

        # Leave the panel blank long enough to see, and answer pings.
        blank_until = time.time() + 1.2
        while time.time() < blank_until:
            for msg in drain(min(time.time() + 0.2, blank_until)):
                if msg.get("type") == "handoff.abort":
                    fail("abort-while-hidden")

        send(
            {
                "v": 1,
                "type": "handoff.return",
                "id": nid("ret"),
                "payload": {},
            }
        )
        shown = wait_type("handoff.shown", 3)
        shown_id = shown.get("id")
        if (shown.get("payload") or {}).get("expression") != "idle":
            fail("shown-not-idle")
        send(
            {
                "v": 1,
                "type": "handoff.ack",
                "id": nid("sack"),
                "payload": {"ref": shown_id},
            }
        )
        # Keep the session up briefly so the restored cat is the return, not a reset.
        hold_until = time.time() + 0.6
        while time.time() < hold_until:
            for msg in drain(min(time.time() + 0.2, hold_until)):
                if msg.get("type") == "handoff.abort":
                    fail("abort-after-shown")
    finally:
        ser.dtr = False
        ser.rts = False
        ser.close()

    print("--- transcript ---")
    for line in transcript:
        print(line)
    print("--- end ---")
    print("RESULT hid-on-commit and shown-on-return")


if __name__ == "__main__":
    main()
