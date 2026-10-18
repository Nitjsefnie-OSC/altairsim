#!/usr/bin/env python3
# Decode CP/M files made by CRUNCH 2.x (a "Z" in the extension: .AZM, .CZM, .DZC ...).
#
#   uncrunch.py [-d DIR] FILE...
#
# Each file is written under the name that is stored in it (SUPSUB13.AZM -> SUPSUB13.ASM).
#
# The format: the bytes 76 FE, the original name and an optional [comment] ending in a 0
# byte, four header bytes (reference revision, significant revision, check type, spare),
# the LZW codes, and a 16-bit checksum of the decoded bytes.
#
# The codes are LZW, most significant bit first, 9 bits wide at the start and up to 12. Codes
# 0-255 are the bytes, 256 ends the file, 257 clears the table, 258 and 259 are fillers to skip.
# When the table is full, CRUNCH does not clear it: it reuses an entry that no code has named
# yet, found through its 5003-slot hash, so the decoder must keep the same hash. The LZW output
# is then run-length coded: 90 n repeats the byte before it n-1 more times, 90 00 is one 90.
#
# Squeezed files (76 FF, .?Q?) are decoded by unsqueeze.py. CRUNCH 1.x files (significant
# revision 1x) and LZH-crunched files (76 FD, .?Y?) are other codings; this tool names them and
# leaves them alone.
#
# Exit status: 0 when every file decoded with a good checksum, 1 otherwise.

import argparse
import os
import sys

MAXCODES = 4096
HASHSIZE = 5003
NONE = -1
EOF, RESET, FILLER1, FILLER2 = 256, 257, 258, 259


class CrunchError(Exception):
    pass


class Bits:
    def __init__(self, data, pos):
        self.data, self.pos, self.box, self.left = data, pos, 0, 0

    def code(self, width):
        value = 0
        for _ in range(width):
            if self.left == 0:
                if self.pos >= len(self.data):
                    raise CrunchError("the file ends before the end code")
                self.box, self.left = self.data[self.pos], 8
                self.pos += 1
            self.left -= 1
            value = (value << 1) | ((self.box >> self.left) & 1)
        return value


class Table:
    def __init__(self):
        self.prefix = [NONE] * MAXCODES
        self.char = [NONE] * MAXCODES
        self.first = [NONE] * MAXCODES
        self.named = [False] * MAXCODES
        self.slots = [NONE] * HASHSIZE
        self.last = -1
        # The 256 bytes and the four special codes go in through the hash too, so that later
        # entries land in the same slots as they did in CRUNCH.
        for c in range(260):
            self.add(0x6FFF if c < 256 else 0x7FFF, c if c < 256 else 0)
            self.named[c] = True

    @staticmethod
    def hash(prefix, char):
        return ((((prefix >> 4) ^ char) | ((prefix & 15) << 8)) + 1)

    def probe(self, start):
        slot = start
        while True:
            yield slot
            slot += start - HASHSIZE
            if slot < 0:
                slot += HASHSIZE

    def link(self, code, prefix, char):
        self.char[code] = char
        if prefix >= MAXCODES:          # a root: one of the 260 starting codes
            self.prefix[code] = NONE
            return
        self.prefix[code] = prefix
        self.first[code] = prefix if self.prefix[prefix] == NONE else self.first[prefix]

    def add(self, prefix, char):
        self.last += 1
        h = self.hash(prefix, char)
        for slot in self.probe(h):
            if self.slots[slot] == NONE:
                self.slots[slot] = self.last
                break
        self.link(self.last, prefix, char)

    def replace(self, prefix, char):
        # The table is full: reuse the first entry on this string's hash chain that no code
        # has named since it was made.
        h = self.hash(prefix, char)
        for slot in self.probe(h):
            code = self.slots[slot]
            if code == NONE:
                return
            if not self.named[code]:
                self.link(code, prefix, char)
                return

    def string(self, code):
        out = []
        while self.prefix[code] != NONE:
            if len(out) >= MAXCODES:
                raise CrunchError("the LZW codes are corrupt")
            out.append(self.char[code])
            code = self.prefix[code]
        out.append(self.char[code])
        if NONE in out:
            raise CrunchError("the LZW codes are corrupt")
        out.reverse()
        return out

    def first_char(self, code):
        if self.prefix[code] != NONE:
            code = self.first[code]
        return self.char[code] if code != NONE else 0


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


def header(data):
    if data[:2] == b"\x76\xfd":
        raise CrunchError("this is an LZH-crunched file (76 FD), which this tool does not read")
    if data[:2] == b"\x76\xff":
        raise CrunchError("this is a squeezed file (76 FF); unsqueeze.py decodes it")
    if data[:2] != b"\x76\xfe":
        raise CrunchError("this is not a crunched file")
    end = data.find(b"\0", 2)
    if end < 0 or end + 5 > len(data):
        raise CrunchError("the header is cut short")
    stored = bytes(b & 0x7F for b in data[2:end]).decode("ascii", "replace")
    revision, checktype = data[end + 2], data[end + 3]
    if revision & 0xF0 != 0x20:
        raise CrunchError("CRUNCH revision %02X; this tool reads only CRUNCH 2.x" % revision)
    # The name may carry a [comment] or a date after it. Keep the 8.3 name only.
    name = stored.split("[")[0].split(" ")[0]
    if "." in name:
        base, ext = name.split(".", 1)
        name = base + "." + ext[:3] if ext[:3] else base
    return name, stored, checktype, end + 5


def uncrunch(data):
    name, stored, checktype, pos = header(data)
    bits = Bits(data, pos)
    table = Table()
    width = 9
    lzw = []
    old = None
    k = 0
    while True:
        code = bits.code(width)
        if code in (FILLER1, FILLER2):
            continue
        if code == EOF:
            break
        add = old is not None
        if code == RESET:
            table, width = Table(), 9
            code = bits.code(width)
            while code in (FILLER1, FILLER2):
                code = bits.code(width)
            if code == EOF:
                break
            add = False
        if code <= table.last:
            lzw.extend(table.string(code))
            k = table.first_char(code)
        elif old is not None:
            # The code being defined right now: the old string plus its own first byte.
            lzw.extend(table.string(old))
            lzw.append(k)
        else:
            raise CrunchError("the LZW codes are corrupt")
        if old is None:
            k = code
        table.named[code] = True
        if table.last == MAXCODES - 1:
            if old is not None:
                table.replace(old, k)
        elif add:
            table.add(old, k)
            if table.last < MAXCODES - 2 and table.last == (1 << width) - 2:
                width += 1
        old = code

    out = unrle(lzw)
    pos = bits.pos
    if checktype == 0:
        if pos + 2 > len(data):
            raise CrunchError("the checksum is missing")
        want = data[pos] | (data[pos + 1] << 8)
        if sum(out) & 0xFFFF != want:
            raise CrunchError("the checksum is wrong")
    return name, stored, out


def valid_name(name):
    if not name or name.startswith("."):
        return False
    return all(32 < ord(c) < 127 and c not in '/\\:*?"<>|' for c in name)


def main():
    ap = argparse.ArgumentParser(description="Decode CP/M files crunched by CRUNCH 2.x.")
    ap.add_argument("-d", "--dir", help="folder to write to (default: the folder of each file)")
    ap.add_argument("file", nargs="+", help="a crunched file (.?Z?)")
    args = ap.parse_args()

    status = 0
    for path in args.file:
        try:
            with open(path, "rb") as f:
                data = f.read()
            name, stored, out = uncrunch(data)
            if not valid_name(name):
                raise CrunchError("the stored name %r is not a file name" % stored)
        except (OSError, CrunchError) as e:
            print("uncrunch: %s: %s" % (path, e), file=sys.stderr)
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
