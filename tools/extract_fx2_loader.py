#!/usr/bin/env python3
"""Write the FX2 stage-1 loader from the OEM loader driver as Intel HEX.

The OEM Windows loader driver (F235Ldr.sys, in the FX35Driver folder of an
OEM install) carries the stage-1 loader the host downloads to the FX2's
internal RAM before the main image (Pakon7.hex): an EZ-Loader record table,
22 bytes per record: u16 length, u16 address, u8 type (0 data, 1 end),
u8 data[16], u8 pad. This finds the table (the longest run of valid records
ending in an end record), drops zero-length records (nothing is sent for
them) and writes the rest as Intel HEX, one line per record, in table order.

That order and those bytes are exactly the stage-1 writes in the firmware
capture resources/f135.pakfw (360 records). The output is OEM firmware:
keep it local (firmware/*.hex is git-ignored) and do not redistribute it.

Usage: extract_fx2_loader.py F235Ldr.sys firmware/PknLdr.hex
"""

import struct
import sys

RECORD = 22


def valid(blob, off):
    if off < 0 or off + RECORD > len(blob):
        return False
    length, addr, typ = struct.unpack_from("<HHB", blob, off)
    return length <= 16 and typ in (0, 1) and addr < 0x4000


def find_table(blob):
    """(records, offset) of the longest valid run that ends in an end record."""
    best = ([], -1)
    for phase in range(RECORD):
        off = phase
        while off + RECORD <= len(blob):
            if not valid(blob, off):
                off += RECORD
                continue
            start, recs = off, []
            while valid(blob, off):
                length, addr, typ = struct.unpack_from("<HHB", blob, off)
                recs.append((addr, typ, blob[off + 5:off + 5 + length]))
                off += RECORD
                if typ == 1:
                    break
            if recs and recs[-1][1] == 1 and len(recs) > len(best[0]):
                best = (recs, start)
    return best


def ihex_line(addr, typ, data):
    body = bytes([len(data), addr >> 8, addr & 0xFF, typ]) + data
    return ":" + (body + bytes([(-sum(body)) & 0xFF])).hex().upper()


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    blob = open(argv[1], "rb").read()
    recs, off = find_table(blob)
    data = [(a, d) for a, t, d in recs if t == 0 and d]
    if len(data) < 16:
        print("no EZ-Loader record table found", file=sys.stderr)
        return 1
    with open(argv[2], "x", encoding="ascii") as out:
        for addr, d in data:
            out.write(ihex_line(addr, 0, d) + "\n")
        out.write(":00000001FF\n")
    print(f"table at 0x{off:x}: {len(data)} records, "
          f"{sum(len(d) for _, d in data)} bytes -> {argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
