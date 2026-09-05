#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
# Copyright 2026 Pharos Tech

"""Transmit the N-Boot zero-delay serial recovery token."""

import argparse
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=1_500_000)
    parser.add_argument("--duration", type=float, default=3.0)
    args = parser.parse_args()

    deadline = time.monotonic() + args.duration
    with serial.Serial(args.port, args.baud, timeout=0) as device:
        while time.monotonic() < deadline:
            device.write(b"!")
            device.flush()
            time.sleep(0.01)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
