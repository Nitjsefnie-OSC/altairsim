# THE COMPUPRO DISK 1 ACCEPTANCE TEST: boot CompuPro CP/M 2.2N off a real 8" disk through the
# board's own boot EPROM, and read and write files on it.
#
# The disk is CompuPro's 2.2N system disk, as raw sectors and not changed (docs/sources.md).
# The boot is routine 4 of builtin:DISK1, a factory EPROM (roms/DISK1/README.md). The EPROM
# reads sectors 1-4 of track 0 (single density, 128 bytes) by DMA. The loader in them reads
# the rest of the system, and the CBIOS reads and writes the double-density tracks
# (8 x 1024) through the 765. tests/test_disk1.cpp checks the board on a hand-built machine;
# this asks whether the machine FILE does it through the real CLI.
#
# IT DRIVES THE GUEST OVER --mcp (the pattern of pb1.cmake): each `run` sends its keys only when
# the one before has stopped at the text it waited for.
#
# What it proves:
#   1. the EPROM boots the disk to the 2.2N sign-on and A>, on the Interfacer I console;
#   2. DIR reads the directory, and STAT DSK: reads the BIOS's disk parameters;
#   3. the write path: PIP copies STAT.COM, the copy runs and prints the same disk parameters,
#      and ERA removes it;
#   4. routine 6 passes C = 2, and the same disk boots to A> on the System Support 1 serial
#      channel (cpm22n-ss1.toml), with the Interfacer not connected.
#
# The guest writes, so the test works on a copy of tests/media/compupro, never the tracked
# image.
#
# Expects: -DSIM=<altairsim> -DSRC=<source dir> -DBIN=<binary dir>

cmake_minimum_required(VERSION 3.16)

set(work "${BIN}/disk1-work")
file(REMOVE_RECURSE "${work}")
file(COPY "${SRC}/tests/media/compupro" DESTINATION "${work}")
set(example "${work}/compupro")

# One tools/call line. `args` is the JSON object of the tool's arguments.
set(session "")
function(call id tool args)
  set(session "${session}{\"jsonrpc\":\"2.0\",\"id\":${id},\"method\":\"tools/call\",\"params\":{\"name\":\"${tool}\",\"arguments\":${args}}}\n" PARENT_SCOPE)
endfunction()

# Type one command at A> and wait for the next A> at the start of a line.
function(cmd id text)
  call(${id} run "{\"input\":\"${text}\\r\",\"until\":\"\\r\\nA>\",\"timeout_ms\":20000}")
  set(session "${session}" PARENT_SCOPE)
endfunction()

set(session "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n")
# MCP does not run the machine file's startup, so start the boot EPROM at 0000 here.
call(2 run "{\"from\":0,\"until\":\"A>\",\"timeout_ms\":20000}")
cmd(3 "DIR")
cmd(4 "STAT DSK:")
cmd(5 "PIP X.COM=STAT.COM")
cmd(6 "X DSK:")
cmd(7 "ERA X.COM")
cmd(8 "DIR X.COM")
file(WRITE "${example}/in.jsonl" "${session}")

execute_process(
  COMMAND           "${SIM}" cpm22n.toml --mcp
  WORKING_DIRECTORY "${example}"
  INPUT_FILE        "${example}/in.jsonl"
  OUTPUT_VARIABLE   out
  ERROR_VARIABLE    err
  RESULT_VARIABLE   rc
  TIMEOUT           110
)

function(fail why)
  message(FATAL_ERROR "disk1: ${why}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endfunction()

if(NOT rc EQUAL 0)
  fail("`altairsim cpm22n.toml --mcp` exited ${rc}, expected 0")
endif()

# The reply to call `id`, one JSON-RPC message per stdout line.
function(reply id var)
  string(REPLACE "\r" "" out_lf "${out}")
  string(REGEX MATCH "(^|\n){[^\n]*\"id\":${id}[,}][^\n]*" line "${out_lf}")
  set(${var} "${line}" PARENT_SCOPE)
endfunction()

# Every run must have stopped because its text arrived, not at the timeout.
foreach(id 2 3 4 5 6 7 8)
  reply(${id} r)
  if(NOT r MATCHES "\"stopped\":\"match\"")
    fail("run ${id} did not come back to A>")
  endif()
endforeach()

# 1. The sign-on of the CBIOS that the EPROM loaded.
reply(2 r)
if(NOT r MATCHES "CompuPro Systems 64K CP/M 2\\.2N")
  fail("the boot did not print the 64K CP/M 2.2N sign-on")
endif()

# 2. The directory and the disk parameters.
reply(3 r)
if(NOT r MATCHES "HMXFPROM ASM" OR NOT r MATCHES "SYSGEN   COM")
  fail("DIR did not list HMXFPROM.ASM and SYSGEN.COM")
endif()
reply(4 r)
if(NOT r MATCHES "600: Kilobyte Drive  Capacity")
  fail("STAT DSK: did not report the 600K drive")
endif()

# 3. The copy that PIP wrote must run and print the same parameters, and ERA must remove it.
reply(6 r)
if(NOT r MATCHES "600: Kilobyte Drive  Capacity")
  fail("X.COM, the copy of STAT.COM that PIP wrote, did not run")
endif()
reply(8 r)
if(NOT r MATCHES "NO FILE")
  fail("ERA did not remove X.COM")
endif()

# 4. Routine 6, on the System Support 1 console. A fresh copy of the disk, so the first
# session's writes do not matter.
file(COPY "${SRC}/tests/media/compupro/CPM22N.DSK" DESTINATION "${example}")
set(session "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n")
call(2 run "{\"from\":0,\"until\":\"A>\",\"timeout_ms\":20000}")
cmd(3 "DIR")
file(WRITE "${example}/in-ss1.jsonl" "${session}")
execute_process(
  COMMAND           "${SIM}" cpm22n-ss1.toml --mcp
  WORKING_DIRECTORY "${example}"
  INPUT_FILE        "${example}/in-ss1.jsonl"
  OUTPUT_VARIABLE   out
  ERROR_VARIABLE    err
  RESULT_VARIABLE   rc
  TIMEOUT           60
)
if(NOT rc EQUAL 0)
  fail("`altairsim cpm22n-ss1.toml --mcp` exited ${rc}, expected 0")
endif()
foreach(id 2 3)
  reply(${id} r)
  if(NOT r MATCHES "\"stopped\":\"match\"")
    fail("routine 6: run ${id} did not come back to A> on the System Support 1")
  endif()
endforeach()
reply(2 r)
if(NOT r MATCHES "CompuPro Systems 64K CP/M 2\\.2N")
  fail("routine 6 did not print the 2.2N sign-on on the System Support 1")
endif()
reply(3 r)
if(NOT r MATCHES "HMXFPROM ASM")
  fail("routine 6: DIR did not list the disk")
endif()

file(REMOVE_RECURSE "${work}")
message(STATUS "disk1: the boot EPROM loaded CompuPro CP/M 2.2N; DIR, STAT, and a PIP "
               "copy that ran and was erased. Routine 6 booted it on the System Support 1.")
