# BASIC 1.0 — the bootstrap source

This folder holds the **source** of the BASIC 1.0 cassette bootstrap:

| File | What it is |
|---|---|
| `LOAD10.ASM` | MITS's own BASIC 1.0 cassette bootstrap (the ACR / 88-SIO variant, I/O 6/7), unmodified. It loads the tape into memory from `0000` and loops forever — no length, no auto-jump — which is why the example boot is two moves (`RUN 1800`, then STOP/RESET, then `RUN 0`). |
| `LOAD10-Octal.PRN` | Its listing. The addresses and bytes in it are in octal. |

`LOAD10.HEX`, the assembled bootstrap, is test media in `tests/media/basic/`, beside the tape it
boots (`acceptance-basic1`).

Both the tape (`BASIC Ver 1-0.tap`) and this loader are from Mike Douglas's paper-tape/cassette
archive at deramp.com (`.../altair/software/papertape_cassette/`), unmodified.
