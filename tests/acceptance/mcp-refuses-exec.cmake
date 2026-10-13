# `--mcp` REFUSES `-x`, `-s` AND `-i` (issue #737).
#
# Under --mcp, main() hands the machine to the MCP server before the code that runs -x and -s
# and reads -i. The options were accepted and dropped: no message, exit status 0. A
# `-x "SET cpu0 clock_hz=2000000"` changed nothing, and a script that did not exist was not
# reported. An option that is accepted and does nothing is the bug, so each one is refused,
# as `--mirror` and `--log` without `--mcp` are:
#   * exit status 2;
#   * a message on stderr that names the option;
#   * nothing on stdout -- that is the JSON-RPC transport, and no session has started.
#
# Stdin is an empty file, so a launch that is NOT refused serves nothing and exits 0 at EOF.
#
# Expects: -DSIM=<altairsim> -DBIN=<binary dir>

cmake_minimum_required(VERSION 3.16)

set(work "${BIN}/mcp-refuses-exec-work")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

file(WRITE "${work}/empty.jsonl" "")
file(WRITE "${work}/real.cmd" "SHOW cpu0\n")

function(fail why)
  message(FATAL_ERROR "mcp-refuses-exec: ${why}\n"
                      "--- exit ---\n${rc}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endfunction()

# refused(<option the message must name> <args...>)
function(refused opt)
  execute_process(
    COMMAND           "${SIM}" ${ARGN}
    WORKING_DIRECTORY "${work}"
    INPUT_FILE        "${work}/empty.jsonl"
    RESULT_VARIABLE   rc
    OUTPUT_VARIABLE   out
    ERROR_VARIABLE    err
    TIMEOUT           30
  )
  string(REPLACE ";" " " shown "${ARGN}")
  if(NOT rc EQUAL 2)
    fail("`altairsim ${shown}` exited ${rc}, expected 2")
  endif()
  if(NOT err MATCHES "${opt} cannot be used with --mcp")
    fail("`altairsim ${shown}` did not say that ${opt} cannot be used with --mcp")
  endif()
  if(NOT out STREQUAL "")
    fail("`altairsim ${shown}` printed on stdout, which is the JSON-RPC transport")
  endif()
endfunction()

# ---- 1. each option, refused ----
refused("-x" altmon --mcp -x "SHOW cpu0")
refused("-s" altmon --mcp -s real.cmd)
refused("-i" altmon --mcp -i)

# The long forms are the same options.
refused("-x" altmon --mcp --exec "SHOW cpu0")
refused("-s" altmon --mcp --script real.cmd)
refused("-i" altmon --mcp --interactive)

# ---- 2. a script that does not exist: still the combination that is refused ----
refused("-s" altmon --mcp -s no-such-file.cmd)

# ---- 3. the order on the command line does not matter ----
refused("-x" -x "SHOW cpu0" altmon --mcp)

# ---- 4. several at once: the message names each one ----
refused("-x, -s, -i" altmon --mcp -x "SHOW cpu0" -s real.cmd -i)

# ---- 5. the control: --mcp alone is still a session ----
execute_process(
  COMMAND           "${SIM}" altmon --mcp
  WORKING_DIRECTORY "${work}"
  INPUT_FILE        "${work}/empty.jsonl"
  RESULT_VARIABLE   rc
  OUTPUT_VARIABLE   out
  ERROR_VARIABLE    err
  TIMEOUT           30
)
if(NOT rc EQUAL 0)
  fail("`altairsim altmon --mcp` exited ${rc}, expected 0")
endif()

message(STATUS "mcp-refuses-exec: PASS")
