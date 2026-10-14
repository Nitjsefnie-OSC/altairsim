# THE COMPUPRO DISK 1A ACCEPTANCE TEST: boot CompuPro CP/M 2.2LD off a real 8" disk through the
# board's own boot EPROM, and read and write files on it.
#
# Nothing here is ours but the machine. The disk is CompuPro's 2.2LD system disk, untouched
# (docs/sources.md), and the boot is routine 0 of the Disk 1A's EPROM (builtin:disk1a). The
# EPROM reads track 0 (single density, 26 x 128) by DMA, and the CBIOS it loads reads and
# writes the double-density tracks (8 x 1024) through the 765. tests/test_disk1a.cpp checks
# the board on a hand-built machine; this asks whether the machine FILE does it through the
# real CLI.
#
# IT DRIVES THE GUEST OVER --mcp (the pattern of pb1.cmake): each `run` sends its keys only when
# the one before has stopped at the text it waited for.
#
# What it proves:
#   1. the EPROM boots the disk to the 2.2LD sign-on and A>;
#   2. DIR reads the directory, and STAT DSK: reads the BIOS's disk parameters;
#   3. the write path: PIP copies STAT.COM, the copy runs and prints the same disk parameters,
#      and ERA removes it.
#
# The guest writes, so the test works on a copy of tests/media/compupro, never the tracked
# image.
#
# Expects: -DSIM=<altairsim> -DSRC=<source dir> -DBIN=<binary dir>

cmake_minimum_required(VERSION 3.16)

set(work "${BIN}/disk1a-work")
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
  COMMAND           "${SIM}" ccpm22ld.toml --mcp
  WORKING_DIRECTORY "${example}"
  INPUT_FILE        "${example}/in.jsonl"
  OUTPUT_VARIABLE   out
  ERROR_VARIABLE    err
  RESULT_VARIABLE   rc
  TIMEOUT           170
)

function(fail why)
  message(FATAL_ERROR "disk1a: ${why}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endfunction()

if(NOT rc EQUAL 0)
  fail("`altairsim ccpm22ld.toml --mcp` exited ${rc}, expected 0")
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
if(NOT r MATCHES "64K CP/M 2\\.2LD")
  fail("the boot did not print the 64K CP/M 2.2LD sign-on")
endif()

# 2. The directory and the disk parameters.
reply(3 r)
if(NOT r MATCHES "SYSGEN   COM" OR NOT r MATCHES "GBCBIOS  ASM")
  fail("DIR did not list SYSGEN.COM and GBCBIOS.ASM")
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

file(REMOVE_RECURSE "${work}")
message(STATUS "disk1a: the boot EPROM loaded CompuPro CP/M 2.2LD; DIR, STAT, and a PIP "
               "copy that ran and was erased.")
