# THE SSM PB1 ACCEPTANCE TEST: run real period EPROM-burner software against the board, under
# the monitor it was written for, and read the burned chip back out as an Intel HEX file
# (issues #397, #382).
#
# This is the whole point of the board (issue #382): not "prepare a ROM image with LOAD ...
# ROM", but RUN the software a 1970s operator ran. The programs in tests/media/pb1 are the SSM
# PB1 manual's own (sections 4.2, 4.4, 4.5), object code verbatim. Each one ends with a jump to
# the SSM 8080 monitor at F021, and the two check programs print P or F through the monitor's
# console-out at F009. The machine file mounts that monitor (builtin:ssm-8080mon), so nothing
# is patched and no breakpoint catches the exit: the test types G<addr> at the monitor's '.'
# prompt and the program comes back to the monitor by itself.
#
# tests/test_pb1.cpp does the same on a hand-built machine. This asks the other question --
# does the machine FILE, copied out of the tree, do it through the real CLI?
#
# IT DRIVES THE GUEST OVER --mcp. A pipe into the console cannot type at a guest (the keys
# race the UART), but a piped MCP session can: the server takes one call at a time, and each
# `run` sends its keys only when the one before has stopped at the text it waited for.
#
# What it proves:
#   1. the monitor boots on the machine's IO-4 console;
#   2. ERACHK prints P for the erased socket, and F after the burn;
#   3. the 2708 burner returns to the monitor, and the burned socket, read back off the bus,
#      is byte-for-byte the source in RAM;
#   4. CPYVFY prints P;
#   5. SAVE turned the socket into a valid Intel HEX file -- the "make a hex file" of #382.
#
# The source pattern does not matter: an erased socket is all-FF, and programming ANDs the
# byte in (FF & x == x), so the socket ends equal to whatever was at 4000 -- random fill and
# all. IF THIS FAILS, THE BOARD OR THE MACHINE FILE IS WRONG, NOT THE SOFTWARE: the programs
# are a faithful, verbatim transcription of the manual.
#
# Expects: -DSIM=<altairsim> -DSRC=<source dir> -DBIN=<binary dir>

cmake_minimum_required(VERSION 3.16)

set(work "${BIN}/pb1-work")
file(REMOVE_RECURSE "${work}")

# Copy ONLY the fixture out of the tree. The monitor is a built-in ROM, so the machine file
# needs nothing beside it but the programs it loads.
file(COPY "${SRC}/tests/media/pb1" DESTINATION "${work}")
set(example "${work}/pb1")

# One tools/call line. `args` is the JSON object of the tool's arguments.
set(session "")
function(call id tool args)
  set(session "${session}{\"jsonrpc\":\"2.0\",\"id\":${id},\"method\":\"tools/call\",\"params\":{\"name\":\"${tool}\",\"arguments\":${args}}}\n" PARENT_SCOPE)
endfunction()

# Type one command at the monitor's '.' prompt and wait for the banner and prompt to come
# back. The monitor prints both each time a program returns through F021.
function(go id cmd)
  call(${id} run "{\"input\":\"${cmd}\\r\",\"until\":\"MONITOR V1.0\\r\\n.\",\"timeout_ms\":15000}")
  set(session "${session}" PARENT_SCOPE)
endfunction()

set(session "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n")
# MCP does not run the machine file's startup, so cold-start the monitor here (F000 = 61440).
call(2 run "{\"from\":61440,\"until\":\"MONITOR V1.0\\r\\n.\",\"timeout_ms\":15000}")
call(3 monitor "{\"command\":\"LOAD PROG2708.HEX\"}")
call(4 monitor "{\"command\":\"LOAD ERACHK.HEX\"}")
call(5 monitor "{\"command\":\"LOAD CPYVFY.HEX\"}")
go(6 "G140")   # erase check on the erased socket      -> P
go(7 "G100")   # the 2708 burner                       -> prints nothing, returns
go(8 "G180")   # verify the socket against the source  -> P
go(9 "G140")   # erase check on the burned socket      -> F
call(10 monitor "{\"command\":\"SAVE eprom.hex D000-D3FF\"}")
call(11 monitor "{\"command\":\"SAVE eprom.bin D000-D3FF FORMAT=BIN\"}")
call(12 monitor "{\"command\":\"SAVE src.bin 4000-43FF FORMAT=BIN\"}")
file(WRITE "${example}/in.jsonl" "${session}")

execute_process(
  COMMAND           "${SIM}" pb1.toml --mcp
  WORKING_DIRECTORY "${example}"
  INPUT_FILE        "${example}/in.jsonl"
  OUTPUT_VARIABLE   out
  ERROR_VARIABLE    err
  RESULT_VARIABLE   rc
  TIMEOUT           100
)

function(fail why)
  message(FATAL_ERROR "pb1: ${why}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endfunction()

if(NOT rc EQUAL 0)
  fail("`altairsim pb1.toml --mcp` exited ${rc}, expected 0")
endif()

# The reply to call `id`, one JSON-RPC message per stdout line.
function(reply id var)
  string(REPLACE "\r" "" out_lf "${out}")
  string(REGEX MATCH "(^|\n){[^\n]*\"id\":${id}[,}][^\n]*" line "${out_lf}")
  set(${var} "${line}" PARENT_SCOPE)
endfunction()

# Every run must have stopped because its text arrived, not at the timeout.
foreach(id 2 6 7 8 9)
  reply(${id} r)
  if(NOT r MATCHES "\"stopped\":\"match\"")
    fail("run ${id} did not come back to the monitor's banner and prompt")
  endif()
endforeach()

# 2 + 4. What the check programs printed, as JSON text: the echoed command, then the letter
# on its own line, then the banner.
reply(6 r)
if(NOT r MATCHES "G140[^\n]*\\\\nP\\\\r\\\\nMONITOR")
  fail("ERACHK did not print P for the erased socket")
endif()
reply(8 r)
if(NOT r MATCHES "G180[^\n]*\\\\nP\\\\r\\\\nMONITOR")
  fail("CPYVFY did not print P after the burn")
endif()
reply(9 r)
if(NOT r MATCHES "G140[^\n]*\\\\nF\\\\r\\\\nMONITOR")
  fail("ERACHK did not print F for the burned socket")
endif()

if(NOT EXISTS "${example}/eprom.hex" OR NOT EXISTS "${example}/eprom.bin" OR NOT EXISTS "${example}/src.bin")
  fail("the burn produced no output files")
endif()

# 3. The burned socket, read back off the bus, must equal the source data exactly.
file(READ "${example}/eprom.bin" burned HEX)
file(READ "${example}/src.bin"   source HEX)
if(NOT burned STREQUAL source)
  fail("the burned 2708 does not match the source data")
endif()

# 5. SAVE must have written a real Intel HEX file: a data record at D000 and the EOF record.
file(READ "${example}/eprom.hex" hextext)
if(NOT hextext MATCHES ":..D000" OR NOT hextext MATCHES ":00000001FF")
  fail("eprom.hex is not a valid Intel HEX of the burn.\n--- eprom.hex ---\n${hextext}")
endif()

file(REMOVE_RECURSE "${work}")
message(STATUS "pb1: under the SSM 8080 monitor, ran the erase check, the 2708 burner and the "
               "verify, and saved the socket as Intel HEX (socket == source).")
