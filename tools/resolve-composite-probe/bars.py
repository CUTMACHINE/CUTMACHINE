#!/usr/bin/env python3
"""The test picture of the composite probe: sixteen grey bars at known
levels, as a 16-bit PPM FFmpeg turns into a clip (README.md)."""

import struct
import sys

LEVELS = [1.0, 0.9, 0.8, 0.749, 0.7, 0.6, 0.561, 0.5, 0.45, 0.4, 0.375,
          0.33, 0.2996, 0.25, 0.15, 0.05]
WIDTH, HEIGHT = 1920, 1080


def main() -> int:
    row = bytearray()
    for x in range(WIDTH):
        value = int(round(LEVELS[x * len(LEVELS) // WIDTH] * 65535))
        row += struct.pack(">HHH", value, value, value)
    with open(sys.argv[1], "wb") as file:
        file.write(b"P6\n%d %d\n65535\n" % (WIDTH, HEIGHT))
        file.write(bytes(row) * HEIGHT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
