#!/usr/bin/env python3
"""DEC15 (GR15) -- writes bodies.txt, the grade bodies resolve_grade_tests reads.

Each body is shaped like a ListMgt::LmVersion.Body of a Resolve project
database, as measured on 20 and 24 October 2026 (docs/roadmap/
GRADE-RESOLVE-2026-10.md, GR15): one byte 0x81 then a zstd frame, or 0x80 then
the bytes, then protobuf. The bodies are written here, field by field; no byte
comes from a client's project, and the values are round numbers.

    python3 tests/fixtures/resolve_grade/make_bodies.py

needs the `zstd` command line (1.5.x). A body named *_stream is compressed
from a pipe, so its frame does not announce its size and the reader's
streaming path is the one exercised. The output is committed: the test does
not need zstd to run.
"""

import pathlib
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent


def varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def field(number, wire, payload):
    return varint(number << 3 | wire) + payload


def v(number, value):
    return field(number, 0, varint(value))


def f32(number, value):
    return field(number, 5, struct.pack("<f", value))


def msg(number, *parts):
    payload = b"".join(parts)
    return field(number, 2, varint(len(payload)) + payload)


def text(number, value):
    data = value.encode("utf-8")
    return field(number, 2, varint(len(data)) + data)


# Values of a parameter (field 2 of a parameter entry).
def float_value(value):
    return msg(2, f32(1, value))


def flag_value(value):
    return msg(2, v(2, value))


def matrix_value(scale):
    return msg(2, msg(10, f32(1, scale), f32(5, scale), f32(9, 1.0)))


def spline_value(points):
    return msg(2, msg(8, *(msg(1, f32(1, x), f32(2, y)) for x, y in points)))


def text_value(value):
    return msg(2, text(5, value))


def parameter(identifier, value):
    return msg(3, v(1, identifier), value)


def corrector(kind, *parameters, enabled=1, blocks=1, frames=None):
    block_list = []
    for index in range(blocks):
        frame = [] if frames is None else [v(1, frames[index])]
        block_list.append(msg(6, *frame, msg(2, v(1, 1), *parameters)))
    head = [v(1, kind)]
    if enabled is not None:
        head.append(v(3, enabled))
    return msg(1, *head, *block_list)


# The correctors every node carries, at rest: types 2, 3, 4, 5, 6 and 18,
# each with its marker -- a flag at 0 or the 3x3 scale between the clip and
# the frame.
def at_rest(scale=1.0):
    return [
        corrector(2, parameter(0x88300031, matrix_value(scale))),
        corrector(3, parameter(0x8870001E, matrix_value(scale))),
        corrector(4, parameter(0x88500010, matrix_value(scale))),
        corrector(5, parameter(0x88B00012, matrix_value(scale))),
        corrector(6, parameter(0x88D00014, matrix_value(scale))),
        corrector(18, parameter(0x88F0000D, matrix_value(scale))),
    ]


def node_properties(*extra):
    return msg(10, msg(1, v(1, 0xC0000001), msg(2, v(2, 2))), *extra)


def node(identifier, number, correctors, label=None, enabled=1, extra=()):
    parts = [v(1, identifier), v(2, number), v(4, 190), v(5, 180)]
    if label is not None:
        parts.append(text(6, label))
    if enabled is not None:
        parts.append(v(7, enabled))
    parts.append(v(8, 44))
    parts.append(msg(9, *correctors, text(2, "\x03\x04\x05\x06\x12")))
    parts.append(node_properties())
    parts.extend(extra)
    parts.append(v(12, 16767174306))
    return msg(7, *parts)


def edge(source, target, identifier):
    return msg(8, v(1, source), v(3, target), v(5, 64), v(6, 64),
               v(7, identifier))


def outside(*correctors):
    return msg(3, v(4, 190), v(5, 180), v(7, 1), v(8, 84),
               msg(9, *correctors, text(2, "\x06")),
               msg(10, msg(1, v(1, 0xC0000001), msg(2, v(2, 2)))),
               v(12, 16643804959))


def grade(nodes, edges=(), outside_block=None):
    resolution = msg(3, v(1, 1920), v(2, 1080), f32(3, 1.0), v(4, 1920),
                     v(5, 1080), f32(6, 1.0), v(7, 1920), v(8, 1080),
                     v(9, 4294967295))
    body = msg(1, v(1, 3), v(2, 1), resolution, *nodes, *edges,
               msg(9, v(1, 1), v(2, 80)), msg(10, v(1, 2), v(2, 64)),
               v(12, 33534348612))
    if outside_block is None:
        outside_block = outside(corrector(6, parameter(0x88D00014,
                                                       matrix_value(0.5625))))
    return body + outside_block


def primaries(*settings):
    return corrector(1, *(parameter(i, float_value(x)) for i, x in settings))


BODIES = {}

# One node: an offset, and a window that holds a shape (the offset applies
# inside it).
BODIES["offset_window"] = grade([node(3, 1, [
    primaries((0x0600007D, -0.25), (0x0600007E, 0.5), (0x0600007F, 0.125)),
    corrector(2, parameter(0x88300001, flag_value(0))),
    corrector(4, parameter(0x08500001, float_value(0.0)),
              parameter(0x08500004, float_value(0.25)),
              parameter(0x88500008, flag_value(0)),
              parameter(0x8850000D, float_value(288.0)),
              parameter(0x88500010, matrix_value(1.0))),
])])

# Two nodes in a chain, listed in the other order: node 5 (number 2) after
# node 3 (number 1). Primaries only, every other corrector at rest.
BODIES["serial_primaries"] = grade(
    [node(5, 2, [primaries((0x06000018, -0.0625), (0x06000019, -0.0625),
                           (0x0600001A, -0.0625), (0x0600001B, 0.03125),
                           (0x06000025, 0.5))] + at_rest(1.7777778),
          label="Peau"),
     node(3, 1, [primaries((0x860000C1, 1.25), (0x860000C0, 0.4375),
                           (0x86000115, -80.0), (0x86000116, -6.5),
                           (0x06000005, 1.125))] + at_rest(1.7777778))],
    [edge(3, 5, 1)])

# A plug-in on the node, an HSL curve, a LUT, a flag nobody has named, and
# the curves' own flag, which says nothing alone.
BODIES["plugin_curve_lut"] = grade([node(3, 1, [
    corrector(1,
              parameter(0x860000C1, float_value(1.5)),
              parameter(0x860000CF, flag_value(2)),
              parameter(0x860000B4, flag_value(2)),
              parameter(0x86000401, spline_value([(-0.5, 0.75),
                                                  (0.25, 0.5)])),
              parameter(0x860000A0, flag_value(6)),
              parameter(0x860000A1, text_value("Fixture/Film to Video.cube"))),
], extra=[msg(10,
              msg(1, v(1, 0xC0000049),
                  msg(2, text(5, "com.blackmagicdesign.resolvefx.vignette"))),
              msg(1, v(1, 0xC0000063), msg(2, v(4, 1))))])])

# Flat HSL curves: the flag stays, no curve.
BODIES["quiet_flags"] = grade([node(3, 1, [
    corrector(1, parameter(0x860000C1, float_value(1.25)),
              parameter(0x860000CF, flag_value(2)),
              parameter(0x860000B8, flag_value(4))),
])])

# Primaries keyframed twice.
BODIES["keyframed"] = grade([node(3, 1, [
    corrector(1, parameter(0x0600007D, float_value(0.25)), blocks=2,
              frames=[172800, 180000]),
])])

# Three nodes, two of them feeding the third: not a chain.
BODIES["parallel"] = grade(
    [node(1, 1, [primaries((0x860000C1, 1.25))]),
     node(2, 2, [primaries((0x06000025, 0.25))]),
     node(3, 3, [])],
    [edge(1, 3, 1), edge(2, 3, 2)])

# Something set outside the nodes (a corrector of type 10).
BODIES["outside_setting"] = grade(
    [node(3, 1, [primaries((0x860000C1, 1.25))])],
    outside_block=outside(corrector(10, parameter(0x10300016,
                                                  float_value(0.5)))))

# A node whose enabled flag is absent, a compound node, and a primary
# corrector switched off.
BODIES["disabled_compound"] = grade(
    [node(1, 1, [primaries((0x860000C1, 1.25))], enabled=None),
     node(2, 2, [corrector(1, parameter(0x860000C1, float_value(1.5)),
                           enabled=0)],
          extra=[msg(11, msg(7, v(1, 9)))])],
    [edge(1, 2, 1)])

# A timeline's grade: a saturation, nothing else.
BODIES["timeline_saturation"] = grade([node(1, 1, [
    primaries((0x06000005, 0.75))])])

# GR16 -- two nodes in a chain that color.resolve_primaries renders whole:
# the gain wheel and its dial, a red gamma, a lift dial and a red offset,
# then a saturation and a Lum Mix of 50.
BODIES["rendered_primaries"] = grade(
    [node(3, 1, [primaries((0x0600001D, 1.25), (0x0600001E, 1.25),
                           (0x0600001F, 1.25), (0x06000020, 1.25),
                           (0x06000022, 0.125), (0x0600001B, -0.0625),
                           (0x0600007D, -0.25))] + at_rest(1.7777778)),
     node(5, 2, [primaries((0x06000005, 1.125), (0x8600000B, 0.5))] +
          at_rest(1.7777778))],
    [edge(3, 5, 1)])

# GR16 -- a contrast and its pivot with a gain: rendered once the project's
# settings are read.
BODIES["contrasted_primaries"] = grade([node(3, 1, [
    primaries((0x860000C1, 1.25), (0x860000C0, 0.40625),
              (0x0600001D, 1.125), (0x0600001E, 1.125),
              (0x0600001F, 1.125), (0x06000020, 1.125))] + at_rest(1.7777778))])


def compress(data, stream):
    command = ["zstd", "-q", "-19", "-c"]
    if stream:
        result = subprocess.run(command, input=data, capture_output=True,
                                check=True)
    else:
        source = HERE / ".body.tmp"
        source.write_bytes(data)
        try:
            result = subprocess.run(command + [str(source)],
                                    capture_output=True, check=True)
        finally:
            source.unlink()
    return result.stdout


def main():
    lines = ["# DEC15 (GR15) -- written by make_bodies.py; name, then the "
             "body in hex."]
    for name, data in BODIES.items():
        lines.append(name + " " + (b"\x81" + compress(data, False)).hex())
    lines.append("serial_primaries_raw " + (b"\x80" +
                                            BODIES["serial_primaries"]).hex())
    lines.append("serial_primaries_stream " + (
        b"\x81" + compress(BODIES["serial_primaries"], True)).hex())
    (HERE / "bodies.txt").write_text("\n".join(lines) + "\n",
                                     encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
