# Running it

`altairsim` is one program file. Unzip the package, and run the program from a terminal.

```
$ ./altairsim
AltairSim X.Y.Z -- 8080, full speed.
machine: default.  HELP for commands.
altairsim>
```

On **Windows**, the program is `altairsim.exe`:

```
> altairsim.exe
```

You can run the program from any folder. Put its folder on your `PATH`, and type `altairsim`.

The `altairsim>` prompt is **the monitor**. The machine has power, but it is stopped. No program
runs until you start one.

## macOS: the first run

The macOS program is signed with a Developer ID, and Apple has checked it (notarized it). macOS
starts it with no message, also when you got the package with a browser.

The first time that you start a new copy, macOS asks Apple about the program. This check needs
the internet. If the Mac is offline, macOS can refuse to start the program. The chapter
*Troubleshooting* tells you what to do.

## Getting help, and getting out

| Type | To |
|---|---|
| `HELP` | list every command |
| `HELP DUMP` | show the usage and examples for one command |
| `QUIT` | leave the program |

**There is no `EXIT` command.** Type `QUIT`, or only `Q`.

You can type a short form of a command. `HELP` shows each command with brackets, for example
`D[UMP]`, `DE[POSIT]` and `RES[ET]`. The part before the bracket is the short form. You can type
the short form, the full name, or any length between them.

**Commands are not case-sensitive.** Board names, board types, settings and their values are
not case-sensitive either. The same is true for endpoints such as `socket:2323` and for
command-line options such as `--mcp`. A file name, a host name and a serial device keep their
case. This manual writes commands in capitals because they are easier to read.

## Which machine you get

When you run `altairsim` with no arguments, you get the machine called `default`. It is a 56K
Altair with a console, a floppy disk controller and a boot PROM. If the folder that you are in
has a file called `altairsim.toml`, you get that machine instead.

To get a different machine, give its name or its file:

```
$ altairsim examples/cpm/cpm22-buffered.toml     a machine file: this one boots CP/M
$ altairsim basic4k                     a BUILT-IN machine, by name
$ altairsim --list                      what the built-in names are
```

A **built-in** machine is a machine file that is stored in the program. It uses the same format
as the machine files in `examples/`. To see what is in a built-in machine, load it and look:

```
$ altairsim basic4k -x BOARDS
```

To get a built-in machine as a file that you can edit, type `CONFIG SAVE mine.toml`. This
command writes the machine that you are running to a file, and that file boots.

The machines chapter describes every command-line option. It also tells you how `altairsim`
decides if a word is the name of a built-in machine or the name of a file.
