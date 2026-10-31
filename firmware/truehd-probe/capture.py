#!/usr/bin/env python3
"""Reads the probe's console from a board's USB-Serial/JTAG port until it prints "result=", and writes it.

    python capture.py COM16 [--seconds 300] [--out file]

The port is opened with DTR and RTS held low (a pulse would reset the board) and reopened when the
chip re-enumerates after a reset. Needs pyserial (the ESP-IDF environment has it).
"""
import argparse
import sys
import time

import serial  # type: ignore


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("--seconds", type=float, default=300.0)
    parser.add_argument("--out")
    args = parser.parse_args()
    deadline = time.monotonic() + args.seconds
    lines: list[str] = []
    done = False
    handle = None
    while time.monotonic() < deadline and not done:
        try:
            if handle is None:
                handle = serial.Serial()
                handle.port = args.port
                handle.baudrate = 115200
                handle.dtr = False
                handle.rts = False
                handle.timeout = 0.5
                handle.open()
            raw = handle.readline()
        except (serial.SerialException, OSError):
            if handle is not None:
                try:
                    handle.close()
                except OSError:
                    pass
            handle = None
            time.sleep(0.3)
            continue
        if not raw:
            continue
        text = raw.decode("utf-8", "replace").rstrip()
        lines.append(text)
        print(text, flush=True)
        if text.startswith("result="):
            done = True
    if args.out:
        with open(args.out, "w", encoding="utf-8") as out:
            out.write("\n".join(lines) + "\n")
    return 0 if done else 2


if __name__ == "__main__":
    sys.exit(main())
