# `--set` CHANGES A SETTING FOR ONE RUN, BEFORE `startup` (issue #736).
#
# `-x "SET cpu0 clock_hz=2000000"` runs AFTER the machine file's `startup` list, so on a
# machine that says `startup = ["RUN FF00"]` the guest has already booted at the old value.
# `--set "cpu0 clock_hz=2000000"` is the same SET, run after the machine is loaded and
# before `startup`. What is checked:
#   * the value is in place when `startup` runs, and the `set>` line is printed before it;
#   * several are applied in the order given;
#   * a setting that fails stops the program with status 2, and `startup` does NOT run --
#     a wrong key must not boot the machine with the wrong setting;
#   * with --mcp it is refused, as -x, -s and -i are (#737).
#
# Stdin is an empty file. Every launch but the last has a -x, so ~/altairsim.ini is not read;
# the last one is a plain launch with a home folder of its own, for the order of the output.
#
# The example in the manual, CP/M at 2 MHz, is checked with the other examples
# (tests/acceptance/examples.cmake): a test of the simulator does not read that tree.
#
# Expects: -DSIM=<altairsim> -DBIN=<binary dir>

cmake_minimum_required(VERSION 3.16)

set(work "${BIN}/set-option-work")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

file(WRITE "${work}/empty.txt" "")
file(WRITE "${work}/cpu.toml"
     "[machine]\nname = \"set-cpu\"\nbase = \"default\"\nstartup = [\"SHOW cpu0\"]\n")
file(WRITE "${work}/sio.toml"
     "[machine]\nname = \"set-sio\"\nbase = \"default\"\nstartup = [\"SHOW sio0\"]\n")

function(fail why)
  message(FATAL_ERROR "set-option: ${why}\n"
                      "--- exit ---\n${rc}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endfunction()

# launch(<args...>) -> rc, out, err in the caller's scope
macro(launch)
  execute_process(
    COMMAND           "${SIM}" ${ARGN}
    WORKING_DIRECTORY "${work}"
    INPUT_FILE        "${work}/empty.txt"
    RESULT_VARIABLE   rc
    OUTPUT_VARIABLE   out
    ERROR_VARIABLE    err
    TIMEOUT           30
  )
endmacro()

# ---- 1. applied before `startup` ----
launch(cpu.toml --set "cpu0 clock_hz=2000000" -x "SHOW MACHINE")
if(NOT rc EQUAL 0)
  fail("a good --set exited ${rc}, expected 0")
endif()
string(FIND "${out}" "set> SET cpu0 clock_hz=2000000" at_set)
string(FIND "${out}" "startup> SHOW cpu0" at_startup)
if(at_set EQUAL -1)
  fail("no `set> SET cpu0 clock_hz=2000000` line on stdout")
endif()
if(at_startup EQUAL -1)
  fail("`startup` did not run after a good --set")
endif()
if(NOT at_set LESS at_startup)
  fail("the `set>` line is after the `startup>` line: --set was applied too late")
endif()
if(NOT out MATCHES "clock_hz +2000000 ")
  fail("the SHOW cpu0 that `startup` ran does not show clock_hz 2000000")
endif()

# ---- 2. the control: without --set, the same machine shows no 2000000 ----
launch(cpu.toml -x "SHOW MACHINE")
if(NOT rc EQUAL 0)
  fail("the control launch exited ${rc}, expected 0")
endif()
if(out MATCHES "clock_hz +2000000 ")
  fail("the control shows clock_hz 2000000 with no --set (case 1 proves nothing)")
endif()

# ---- 3. more than one, in the order given: the last one wins ----
launch(cpu.toml --set "cpu0 clock_hz=2000000" --set "cpu0 clock_hz=4000000" -x "SHOW MACHINE")
if(NOT rc EQUAL 0)
  fail("two --set exited ${rc}, expected 0")
endif()
if(NOT out MATCHES "clock_hz +4000000 " OR out MATCHES "clock_hz +2000000 ")
  fail("two --set for one key: `startup` did not see the last value")
endif()

# ---- 4. a unit of a board ----
launch(sio.toml --set "sio0:b baud=1200" -x "SHOW MACHINE")
if(NOT rc EQUAL 0)
  fail("--set on a unit exited ${rc}, expected 0")
endif()
if(NOT out MATCHES "set> SET sio0:b baud=1200")
  fail("no `set>` line for the unit")
endif()
if(NOT out MATCHES "baud +1200 ")
  fail("the SHOW sio0 that `startup` ran does not show baud 1200")
endif()

# ---- 5. a wrong key: status 2, the error on stderr, and `startup` does not run ----
launch(cpu.toml --set "cpu0 bogus=1" -x "SHOW MACHINE")
if(NOT rc EQUAL 2)
  fail("a wrong key exited ${rc}, expected 2")
endif()
if(NOT err MATCHES "bogus")
  fail("a wrong key: stderr does not name it")
endif()
if(NOT err MATCHES "--set failed")
  fail("a wrong key: stderr does not say that --set failed")
endif()
if(out MATCHES "startup>" OR out MATCHES "altairsim>")
  fail("a wrong key: the program went on to run `startup` or -x")
endif()

# ---- 6. a good one, then a wrong one: the same ----
launch(cpu.toml --set "cpu0 clock_hz=2000000" --set "cpu0 bogus=1" -x "SHOW MACHINE")
if(NOT rc EQUAL 2)
  fail("a good --set then a wrong one exited ${rc}, expected 2")
endif()
if(out MATCHES "startup>" OR out MATCHES "altairsim>")
  fail("a good --set then a wrong one: the program went on to run `startup` or -x")
endif()

# ---- 7. no argument ----
launch(cpu.toml --set)
if(NOT rc EQUAL 2)
  fail("--set with no argument exited ${rc}, expected 2")
endif()
if(NOT err MATCHES "--set needs")
  fail("--set with no argument: stderr does not say what it needs")
endif()

# ---- 8. with --mcp it is refused, and stdout (the JSON-RPC transport) stays empty ----
launch(cpu.toml --mcp --set "cpu0 clock_hz=2000000")
if(NOT rc EQUAL 2)
  fail("--set with --mcp exited ${rc}, expected 2")
endif()
if(NOT err MATCHES "--set cannot be used with --mcp")
  fail("--set with --mcp: stderr does not say that it cannot be used with --mcp")
endif()
if(NOT out STREQUAL "")
  fail("--set with --mcp printed on stdout, which is the JSON-RPC transport")
endif()

# ---- 9. a plain launch: banner, then ~/altairsim.ini, then --set, then `startup` ----
# The banner reports the machine as the file made it (full speed here). The ini file and then
# the command line change it, so a --set wins over the same key in the ini file.
file(MAKE_DIRECTORY "${work}/home")
file(WRITE "${work}/home/altairsim.ini" "SET cpu0 clock_hz=1000000\n")
file(WRITE "${work}/quit.txt" "QUIT\n")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "HOME=${work}/home" "USERPROFILE=${work}/home"
          "${SIM}" cpu.toml --set "cpu0 clock_hz=2000000"
  WORKING_DIRECTORY "${work}"
  INPUT_FILE        "${work}/quit.txt"
  RESULT_VARIABLE   rc
  OUTPUT_VARIABLE   out
  ERROR_VARIABLE    err
  TIMEOUT           30
)
if(NOT rc EQUAL 0)
  fail("a plain launch with --set exited ${rc}, expected 0")
endif()
string(FIND "${out}" "full speed." at_banner)
string(FIND "${out}" "ini> SET cpu0 clock_hz=1000000" at_ini)
string(FIND "${out}" "set> SET cpu0 clock_hz=2000000" at_set)
string(FIND "${out}" "startup> SHOW cpu0" at_startup)
if(at_banner EQUAL -1 OR at_ini EQUAL -1 OR at_set EQUAL -1 OR at_startup EQUAL -1)
  fail("a plain launch: the banner (full speed), `ini>`, `set>` or `startup>` line is missing")
endif()
if(NOT at_banner LESS at_ini OR NOT at_ini LESS at_set OR NOT at_set LESS at_startup)
  fail("a plain launch: the order is not banner, ini>, set>, startup>")
endif()
if(NOT out MATCHES "clock_hz +2000000 ")
  fail("a plain launch: --set did not win over the same key in ~/altairsim.ini")
endif()

message(STATUS "set-option: PASS")
