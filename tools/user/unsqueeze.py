#!/usr/bin/env python3
# Decode CP/M files made by SQ, the squeezer (a "Q" in the extension: .AQM, .CQM, .DQC ...).
#
#   unsqueeze.py [-d DIR] FILE...
#
# Each file is written under the name that is stored in it (EX15.AQM -> EX15.ASM).
#
# The format: the bytes 76 FF, a 16-bit checksum of the decoded bytes, the original name
# ending in a 0 byte, a 16-bit count of tree nodes (257 at most), then the nodes, each two
# signed 16-bit children. A child of 0 or more is the index of another node. A negative child
# is a leaf that holds the value -(child + 1). Value 256 ends the file. All numbers are low
# byte first. The coded bits follow, least significant bit of each byte first: from node 0,
# a 0 bit takes the first child and a 1 bit the second, until a leaf. The values are then
# run-length coded: 90 n repeats the byte before it n-1 more times, 90 00 is one 90.
#
# Crunched files (76 FE, .?Z?) are decoded by uncrunch.py. LZH-crunched files (76 FD, .?Y?)
# are another coding; this tool names them and leaves them alone.
#
# Exit status: 0 when every file decoded with a good checksum, 1 otherwise.

import argparse
import os
import sys

MAXNODES = 257
SPEOF = 256


class SqueezeError(Exception):
    pass


def unrle(stream):
    out = bytearray()
    last = 0
    repeating = False
    for c in stream:
        if repeating:
            if c == 0:
                out.append(0x90)
            else:
                out.extend(bytes([last]) * (c - 1))
            repeating = False
        elif c == 0x90:
            repeating = True
        else:
            out.append(c)
            last = c
    return bytes(out)


def word(data, pos, signed=False):
    if pos + 2 > len(data):
        raise SqueezeError("the file is cut short")
    return int.from_bytes(data[pos:pos + 2], "little", signed=signed)


def unsqueeze(data):
    if data[:2] == b"\x76\xfe":
        raise SqueezeError("this is a crunched file (76 FE); uncrunch.py decodes it")
    if data[:2] == b"\x76\xfd":
        raise SqueezeError("this is an LZH-crunched file (76 FD), which this tool does not read")
    if data[:2] != b"\x76\xff":
        raise SqueezeError("this is not a squeezed file")
    want = word(data, 2)
    end = data.find(b"\0", 4)
    if end < 0:
        raise SqueezeError("the header is cut short")
    stored = bytes(b & 0x7F for b in data[4:end]).decode("ascii", "replace")
    pos = end + 1
    count = word(data, pos)
    pos += 2
    if count > MAXNODES:
        raise SqueezeError("the tree has %d nodes; the most is %d" % (count, MAXNODES))
    tree = []
    for _ in range(count):
        tree.append((word(data, pos, True), word(data, pos + 2, True)))
        pos += 4

    values = []
    if count:
        bitpos = pos * 8
        limit = len(data) * 8
        while True:
            node = 0
            while node >= 0:
                if node >= count:
                    raise SqueezeError("the tree is corrupt")
                if bitpos >= limit:
                    raise SqueezeError("the file ends before the end code")
                bit = (data[bitpos >> 3] >> (bitpos & 7)) & 1
                bitpos += 1
                node = tree[node][bit]
            value = -(node + 1)
            if value == SPEOF:
                break
            if value > SPEOF:
                raise SqueezeError("the tree is corrupt")
            values.append(value)

    out = unrle(values)
    if sum(out) & 0xFFFF != want:
        raise SqueezeError("the checksum is wrong")

    # The name may carry a [comment] or a date after it. Keep the 8.3 name only.
    name = stored.split("[")[0].split(" ")[0]
    if "." in name:
        base, ext = name.split(".", 1)
        name = base + "." + ext[:3] if ext[:3] else base
    return name, stored, out


def valid_name(name):
    if not name or name.startswith("."):
        return False
    return all(32 < ord(c) < 127 and c not in '/\\:*?"<>|' for c in name)


def main():
    ap = argparse.ArgumentParser(description="Decode CP/M files squeezed by SQ.")
    ap.add_argument("-d", "--dir", help="folder to write to (default: the folder of each file)")
    ap.add_argument("file", nargs="+", help="a squeezed file (.?Q?)")
    args = ap.parse_args()

    status = 0
    for path in args.file:
        try:
            with open(path, "rb") as f:
                data = f.read()
            name, stored, out = unsqueeze(data)
            if not valid_name(name):
                raise SqueezeError("the stored name %r is not a file name" % stored)
        except (OSError, SqueezeError) as e:
            print("unsqueeze: %s: %s" % (path, e), file=sys.stderr)
            status = 1
            continue
        folder = args.dir if args.dir is not None else os.path.dirname(path)
        if folder:
            os.makedirs(folder, exist_ok=True)
        target = os.path.join(folder, name)
        with open(target, "wb") as f:
            f.write(out)
        st = os.stat(path)
        os.utime(target, (st.st_atime, st.st_mtime))
        print("%s -> %s" % (path, target))
    return status


if __name__ == "__main__":
    sys.exit(main())
