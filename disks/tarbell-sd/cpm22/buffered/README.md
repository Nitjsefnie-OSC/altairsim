# Tarbell SD — CP/M 2.2b (track-buffered), single density

The Tarbell controller is the `tarbell` board (`docs/boards/tarbell-sd.md`), and `tarbell` is a
built-in machine. To boot this disk:

```
altairsim tarbell
MOUNT fdc0:drive0 CPM22b15-48K-SSSD.DSK
RUN 0
```

The machine prints `48K CP/M 2.2b v1.5` and `A>`. A copy of the disk is tracked at
`tests/media/tarbell/CPM22b15-48K-SSSD.DSK`, and `tests/acceptance/tarbell.exp` boots it.

There is no machine file in this directory, because the built-in machine is the machine file.
The directory also keeps `BIOS (1).ASM`, because that file is a **source**, and it is cited.

## Not in this repository

`*.DSK` is in `.gitignore`. **Download from:**

> <https://deramp.com/downloads/altair/software/tarbell_floppy_controllers/single_density_controller/CPM%202.2B%20(track%20buffered)/>

| File | What it is |
|---|---|
| `CPM22b15-48K-SSSD.DSK` | Bootable Tarbell CP/M 2.2b, 48K. The `tarbell` machine boots it. |
| `BOOT.ASM`, `IOBBIOS.ASM`, `IOBLOAD.ASM` | Further listings, not currently cited. |
| `ReadMe.pdf` | Vendor documentation. |

## What is tracked, and why

Only `README.md` and **`BIOS (1).ASM`**.

That one file stays in the tree because `docs/boards/tarbell-sd.md` cites it as a source — it is what
settled *why the BIOS never writes `0xFC`*, which had looked like a hole in the listing. Pad **E30 is
tied to ground** (manual p.24) and may be strapped to E29 to permanently select drive 0, so a
single-drive Tarbell has no reason to touch the control port at all. **The BIOS is not incomplete —
the board is strapped.** That is a first-hand hardware fact recovered from a period artifact
(`DESIGN.md` §0.1), and it would be lost if the file went.

## The trap to remember

**The Tarbell numbers its sectors from 1. The 88-DCDD numbers them from 0.** `DESIGN.md` §7.3 calls
that the off-by-one that *silently corrupts a disk*. Both boards can be in the same machine, with
both conventions live. Keep that in mind when you change either board.
