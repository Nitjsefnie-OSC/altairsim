#!/usr/bin/env python3
# List or extract the member files of a CP/M library (.LBR) file.
#
#   lbr-extract.py [-l] [-d DIR] FILE.LBR [MEMBER...]
#
# An LBR file is a set of 128-byte sectors. The first sectors are the directory: 32-byte
# entries, the first of which describes the directory itself. Each entry gives the member's
# name, its first sector, its length in sectors, a CRC-16 (CCITT, as XMODEM uses; 0 = no CRC)
# and the number of unused bytes in its last sector. The member is written exactly as it is
# stored -- a crunched member (.?Z?) stays crunched; uncrunch.py decodes it.
#
# Exit status: 0 when every member was listed or written with a good CRC, 1 otherwise.

import argparse
import os
import struct
import sys
import time

SECTOR = 128
ENTRY = 32
ACTIVE, DELETED, UNUSED = 0x00, 0xFE, 0xFF


class LbrError(Exception):
    pass


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def cpm_name(raw):
    # Eight name bytes and three extension bytes, space padded. The high bits are CP/M's
    # file attributes, not part of the name.
    name = bytes(b & 0x7F for b in raw[:8]).decode("ascii", "replace").rstrip()
    ext = bytes(b & 0x7F for b in raw[8:11]).decode("ascii", "replace").rstrip()
    return name + "." + ext if ext else name


def valid_name(name):
    # A plain 8.3 name: nothing a host would read as a path.
    if not name or name.startswith("."):
        return False
    return all(32 < ord(c) < 127 and c not in '/\\:*?"<>|' for c in name)


def cpm_mtime(date, hhmmss):
    # LBR dates count days from 31 December 1977 (day 1 = 1 January 1978). The time is in
    # MS-DOS form: hours in bits 15-11, minutes 10-5, seconds/2 4-0. 0 = no date.
    if date == 0:
        return None
    day = time.gmtime(252374400 + date * 86400)
    return time.mktime((day.tm_year, day.tm_mon, day.tm_mday,
                        hhmmss >> 11, (hhmmss >> 5) & 63, (hhmmss & 31) * 2, 0, 0, -1))


def read_directory(data):
    if len(data) < SECTOR:
        raise LbrError("file is shorter than one sector")
    status, index, sectors = data[0], *struct.unpack_from("<HH", data, 12)
    if status != ACTIVE or data[1:12] != b" " * 11 or index != 0 or sectors == 0:
        raise LbrError("not an LBR file (the first entry is not a directory)")
    if sectors * SECTOR > len(data):
        raise LbrError("the directory is longer than the file")
    directory = bytearray(data[:sectors * SECTOR])
    stored = struct.unpack_from("<H", directory, 16)[0]
    if stored:
        directory[16:18] = b"\0\0"
        if crc16(directory) != stored:
            raise LbrError("the directory CRC is wrong")

    members = []
    for off in range(ENTRY, len(directory), ENTRY):
        e = directory[off:off + ENTRY]
        if e[0] != ACTIVE:
            continue
        index, sectors, crc, cdate, mdate, ctime, mtime_ = struct.unpack_from("<7H", e, 12)
        if mdate == 0:
            mdate, mtime_ = cdate, ctime
        pad = e[26]
        size = sectors * SECTOR - pad if sectors else 0
        if pad >= SECTOR or (index + sectors) * SECTOR > len(data):
            raise LbrError("member %s lies outside the file" % cpm_name(e[1:12]))
        members.append({
            "name": cpm_name(e[1:12]),
            "start": index * SECTOR,
            "sectors": sectors,
            "size": size,
            "crc": crc,
            "mtime": cpm_mtime(mdate, mtime_),
        })
    return members


def main():
    ap = argparse.ArgumentParser(description="List or extract the members of a CP/M .LBR file.")
    ap.add_argument("-l", "--list", action="store_true", help="list the members; write nothing")
    ap.add_argument("-d", "--dir", default=".", help="folder to write the members to (default: .)")
    ap.add_argument("lbr", help="the .LBR file")
    ap.add_argument("member", nargs="*", help="extract only these members (default: all)")
    args = ap.parse_args()

    try:
        with open(args.lbr, "rb") as f:
            data = f.read()
        members = read_directory(data)
    except (OSError, LbrError) as e:
        print("lbr-extract: %s: %s" % (args.lbr, e), file=sys.stderr)
        return 1

    wanted = {m.upper() for m in args.member}
    found = set()
    status = 0
    if not args.list:
        os.makedirs(args.dir, exist_ok=True)
    for m in members:
        if wanted and m["name"].upper() not in wanted:
            continue
        found.add(m["name"].upper())
        # The CRC covers the whole member as stored, padding included.
        body = data[m["start"]:m["start"] + m["sectors"] * SECTOR]
        if m["crc"] == 0:
            check = "no CRC"
        elif crc16(body) == m["crc"]:
            check = "CRC ok"
        else:
            check = "CRC WRONG"
            status = 1
        if args.list:
            print("%-12s %7d  %s" % (m["name"], m["size"], check))
            continue
        if check == "CRC WRONG":
            print("lbr-extract: %s: the CRC is wrong; not written" % m["name"], file=sys.stderr)
            continue
        if not valid_name(m["name"]):
            print("lbr-extract: %r is not a file name; not written" % m["name"], file=sys.stderr)
            status = 1
            continue
        out = os.path.join(args.dir, m["name"])
        with open(out, "wb") as f:
            f.write(body[:m["size"]])
        if m["mtime"] is not None:
            os.utime(out, (m["mtime"], m["mtime"]))
        print("%s -> %s" % (m["name"], out))

    for name in sorted(wanted - found):
        print("lbr-extract: %s is not in %s" % (name, args.lbr), file=sys.stderr)
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
