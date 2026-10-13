---
name: altairsim-cpm-text
description: Rules for a text file that a CP/M program will read on an altairsim machine — CR/LF line ends, the 8.3 name, and the Ctrl-Z that ends the file. Use this whenever you create or edit a file on the host that goes into CP/M (an `.ASM` or `.MAC` source, a `.BAS` program, a `.SUB` or `.TXT` file), whenever you bring a text file back from CP/M, and when an assembly makes nothing or a BASIC program arrives as one line.
---

# Text files for CP/M

This skill needs the `altairsim` skill, which tells you how to boot a machine and type at it.
Each rule here was replayed on `examples/cpm/cpm22-buffered.toml`.

A file goes into the guest as it is. The host bridge (`R`) and `PASTE` do not change one
byte. For this reason, the file must be correct for CP/M before you send it.

## The rules

1. **End every line with CR/LF.** CP/M programs read CR/LF (`0D 0A`) as the end of a line. A
   file with LF line ends has no line ends for them.
2. **Check the line ends after each edit.** Many host editors and edit tools write LF. A file
   that was correct can change when you save it.
3. **Give the file an 8.3 name in upper case.** CP/M has eight characters for the name and
   three for the extension. `R` changes a longer host name: `my-notes(2).txt` becomes
   `MY-NOTES.TXT`. Give the name that you want as the second argument: `R my-notes(2).txt NOTES.TXT`.
4. **Know which files come back with extra bytes at the end.** CP/M stores files in 128-byte
   records. A text file ends at the first Ctrl-Z (`1A`). CP/M does not fill the remainder of
   the last record: the bytes after that Ctrl-Z can be more `1A` or old data. For the
   extensions `ASM` `MAC` `Z80` `HEX` `PRN` `LST` `SYM` `INC` `SUB` `TXT`, `W NAME.TXT` stops
   at the first Ctrl-Z, and the host file is clean text. For a text file with another
   extension, such as `.BAS`, add the `T`: `W PROG.BAS PROG.BAS T`. Without it, the host file
   has up to 127 extra bytes at its end.

## Check and correct the line ends on the host

```sh
file HELLO.ASM                              # must say "with CRLF line terminators"
perl -pi -e 's/\r?\n/\r\n/' HELLO.ASM       # changes LF to CR/LF, and leaves CR/LF as it is
```

## How a file with LF line ends fails

The failure has no clear message. These are the signs:

| Program | What you see |
|---|---|
| `ASM` | The line after the banner is `0000`, or correct source lines get an `E` or `S` error. The `.PRN` file is empty or has lines without an address. |
| `MBASIC`, typed or pasted | All the lines become one program line. `RUN` gives `Syntax error` in that line. |
| `MBASIC`, `LOAD "NAME"` | `LOAD` prints `Ok`, and `LIST` then shows nothing. |

When you see one of these signs, check the line ends before you change the source.
