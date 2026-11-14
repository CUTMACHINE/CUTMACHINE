"""FI4 -- ROADMAP.md: give the bundled upright font a private test family.

Core Text also sees installed fonts. Registering just Fira's upright files
does not hide an italic installed by the user, so the missing-italic case
needs a family whose only face belongs to the test. Keep all font tables
and licence records; change the names and recompute the sfnt checksums.
"""

import struct
import sys
from pathlib import Path


def checksum(data):
    padded = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(padded) // 4}I", padded)) & 0xffffffff


def private_names(table):
    version, count, start = struct.unpack_from(">HHH", table)
    if version != 0:
        raise ValueError("test fixture needs a format-0 name table")
    family = "CUTMACHINE Test Upright"
    names = {1: family, 3: family, 4: family, 6: "CUTMACHINETestUpright",
             16: family, 18: family, 21: family}
    records = bytearray()
    strings = bytearray()
    for index in range(count):
        platform, encoding, language, name, size, offset = struct.unpack_from(
            ">6H", table, 6 + index * 12)
        value = table[start + offset:start + offset + size]
        if name in names:
            value = names[name].encode("utf-16-be" if platform in (0, 3) else "mac_roman")
        records.extend(struct.pack(">6H", platform, encoding, language, name,
                                   len(value), len(strings)))
        strings.extend(value)
    return struct.pack(">HHH", 0, count, 6 + len(records)) + records + strings


def make_fixture(source, destination):
    data = Path(source).read_bytes()
    count = struct.unpack_from(">H", data, 4)[0]
    tables = []
    for index in range(count):
        tag, _, offset, size = struct.unpack_from(">4sIII", data, 12 + index * 16)
        table = data[offset:offset + size]
        if tag == b"name":
            table = private_names(table)
        if tag == b"head":
            table = table[:8] + b"\0\0\0\0" + table[12:]
        tables.append((tag, table))
    output = bytearray(data[:12] + b"\0" * (16 * count))
    head = None
    for index, (tag, table) in enumerate(tables):
        offset = len(output)
        struct.pack_into(">4sIII", output, 12 + index * 16, tag,
                         checksum(table), offset, len(table))
        if tag == b"head":
            head = offset
        output.extend(table)
        output.extend(b"\0" * (-len(table) % 4))
    if head is None:
        raise ValueError("test fixture has no head table")
    struct.pack_into(">I", output, head + 8,
                     (0xB1B0AFBA - checksum(output)) & 0xffffffff)
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(output)


if __name__ == "__main__":
    make_fixture(*sys.argv[1:3])
