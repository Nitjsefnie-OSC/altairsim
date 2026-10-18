# THE LBR TOOLS OPEN A REAL LIBRARY.
#
# tools/user/lbr-extract.py, uncrunch.py and unsqueeze.py ship as tools/ in the package. This
# runs them on two period libraries in tests/media/lbr: SUPSUB13.LBR, three files crunched by
# CRUNCH 2.4, and EX15.LBR, eight files squeezed by SQ. It checks:
#   1. `-l` lists the three members with a good CRC, and extract writes them as stored;
#   2. uncrunch writes SUPSUB13.ASM, .COM and .DOC, and each one has the SHA-256 below. The
#      values are from lbrate 1.1 (Russell Marks), an independent decoder, not from these tools.
#      SUPSUB13.AZM fills the LZW table, so the code that reuses table entries is in the test;
#   3. one flipped bit in a member: extract reports the CRC and writes nothing for it, exit 1;
#   4. one flipped bit in a crunched file: uncrunch reports it and writes nothing, exit 1;
#   5. EX15.LBR: extract, then unsqueeze writes the eight files, each with lbrate's SHA-256;
#   6. one flipped bit in a squeezed file: unsqueeze reports it and writes nothing, exit 1.
#
# Expects: -DPYTHON=<python3> -DSRC=<source dir> -DBIN=<binary dir>

cmake_minimum_required(VERSION 3.16)

set(work "${BIN}/lbr-tools-work")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/raw" "${work}/out" "${work}/bad")

set(lbr     "${SRC}/tests/media/lbr/SUPSUB13.LBR")
set(extract "${SRC}/tools/user/lbr-extract.py")
set(uncr    "${SRC}/tools/user/uncrunch.py")
set(unsq    "${SRC}/tools/user/unsqueeze.py")
set(ex15    "${SRC}/tests/media/lbr/EX15.LBR")

function(fail why)
  message(FATAL_ERROR "lbr-tools: ${why}\n"
                      "--- exit ---\n${rc}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endfunction()

macro(run)
  execute_process(COMMAND "${PYTHON}" -I ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
endmacro()

# flip(<in> <out> <offset>) -- copy <in> to <out> with bit 0 of the byte at <offset> inverted.
macro(flip in outfile offset)
  # Newlines, not semicolons: a semicolon would split the argument into a CMake list.
  run(-c "import sys\nd = bytearray(open(sys.argv[1], 'rb').read())\nd[int(sys.argv[3])] ^= 1\nopen(sys.argv[2], 'wb').write(d)"
      "${in}" "${outfile}" "${offset}")
  if(NOT rc EQUAL 0)
    fail("could not make ${outfile}")
  endif()
endmacro()

# ---- 1. list and extract ----
run("${extract}" -l "${lbr}")
if(NOT rc EQUAL 0)
  fail("-l exited ${rc}")
endif()
foreach(line "SUPSUB13.AZM +10880  CRC ok" "SUPSUB13.CZM +1920  CRC ok" "SUPSUB13.DZC +3072  CRC ok")
  if(NOT out MATCHES "${line}")
    fail("-l did not print `${line}`")
  endif()
endforeach()

run("${extract}" -d "${work}/raw" "${lbr}")
if(NOT rc EQUAL 0)
  fail("extract exited ${rc}")
endif()
foreach(member SUPSUB13.AZM SUPSUB13.CZM SUPSUB13.DZC)
  if(NOT EXISTS "${work}/raw/${member}")
    fail("extract did not write ${member}")
  endif()
endforeach()

# ---- 2. uncrunch, and compare with lbrate's output ----
run("${uncr}" -d "${work}/out" "${work}/raw/SUPSUB13.AZM" "${work}/raw/SUPSUB13.CZM"
    "${work}/raw/SUPSUB13.DZC")
if(NOT rc EQUAL 0)
  fail("uncrunch exited ${rc}")
endif()
foreach(pair
    "SUPSUB13.ASM=ab621cfb40211a244aa0ea8e0271d21e37ccb61568619c765e243493300c09dd"
    "SUPSUB13.COM=e00022e76f0b9f338bc492dd864a21e1031f36bbcf77c27799b5ed782bf08dc8"
    "SUPSUB13.DOC=37790441fbadf2fa5b113671edf45f23eb3c82e2276789053322592d325cd77f")
  string(REPLACE "=" ";" pair "${pair}")
  list(GET pair 0 name)
  list(GET pair 1 want)
  if(NOT EXISTS "${work}/out/${name}")
    fail("uncrunch did not write ${name}")
  endif()
  file(SHA256 "${work}/out/${name}" got)
  if(NOT got STREQUAL want)
    fail("${name} has SHA-256 ${got}, expected ${want}")
  endif()
endforeach()

# ---- 3. a bad member CRC ----
# Byte 1000 is in the first member (SUPSUB13.AZM starts at sector 1, byte 128).
flip("${lbr}" "${work}/bad/BAD.LBR" 1000)
run("${extract}" -d "${work}/bad" "${work}/bad/BAD.LBR")
if(NOT rc EQUAL 1)
  fail("extract of a bad member exited ${rc}, expected 1")
endif()
if(NOT err MATCHES "SUPSUB13.AZM: the CRC is wrong; not written")
  fail("extract did not report the bad CRC of SUPSUB13.AZM")
endif()
if(EXISTS "${work}/bad/SUPSUB13.AZM")
  fail("extract wrote SUPSUB13.AZM although its CRC is wrong")
endif()
if(NOT EXISTS "${work}/bad/SUPSUB13.CZM")
  fail("extract did not write the good member SUPSUB13.CZM")
endif()

# ---- 4. a bad crunched file ----
file(MAKE_DIRECTORY "${work}/bad2")
flip("${work}/raw/SUPSUB13.AZM" "${work}/bad2/SUPSUB13.AZM" 5000)
run("${uncr}" "${work}/bad2/SUPSUB13.AZM")
if(NOT rc EQUAL 1)
  fail("uncrunch of a bad file exited ${rc}, expected 1")
endif()
if(NOT err MATCHES "uncrunch: .*SUPSUB13.AZM: the (checksum is wrong|LZW codes are corrupt)")
  fail("uncrunch did not report the bad file")
endif()
if(EXISTS "${work}/bad2/SUPSUB13.ASM")
  fail("uncrunch wrote SUPSUB13.ASM from a bad file")
endif()

# ---- 5. a squeezed library ----
file(MAKE_DIRECTORY "${work}/sq/raw" "${work}/sq/out" "${work}/sq/bad")
run("${extract}" -d "${work}/sq/raw" "${ex15}")
if(NOT rc EQUAL 0)
  fail("extract of EX15.LBR exited ${rc}")
endif()
set(squeezed READ.MQ EX15.AQM EX15.CQM EX15.DQC EX15.LQB EX15.SQB EX15.TQT RELS.UQL)
list(TRANSFORM squeezed PREPEND "${work}/sq/raw/")
run("${unsq}" -d "${work}/sq/out" ${squeezed})
if(NOT rc EQUAL 0)
  fail("unsqueeze exited ${rc}")
endif()
foreach(pair
    "READ.ME=33ba6f74ad7f2558acf3ef28bc09918a0ee68de45ab64b2db22ea2dc8b5c6b7f"
    "EX15.ASM=6d3d7535d1703983bb44c30259af492249754b8f712414c02f12b7ea68ee0b9f"
    "EX15.COM=b2ba2fdc791a7daf41d7932b476da82551a5c37fd88132d3fa8fb523af829b2a"
    "EX15.DOC=6eeae8bec2bff6b9beb366f837bf68c93f63a23267cd87332726de6af741139c"
    "EX15.LIB=00ac2f6aad14e003c13ce817a6c5bdf92e084fb18b9c07719f72c14d926153f1"
    "EX15.SUB=343e6945cbef22027c7fd6afe0e594a99b9d4fa94b44f61d65704301a25bee58"
    "EX15.TST=cde3777f85816308c40a172d597f095f92d83356a9b5ae932b3c8cf4e90f8562"
    "RELS.UTL=d1395c1ffa3e74d22e29bd26b8e68c2195725136c02f47181bb9772f8d1c18ab")
  string(REPLACE "=" ";" pair "${pair}")
  list(GET pair 0 name)
  list(GET pair 1 want)
  if(NOT EXISTS "${work}/sq/out/${name}")
    fail("unsqueeze did not write ${name}")
  endif()
  file(SHA256 "${work}/sq/out/${name}" got)
  if(NOT got STREQUAL want)
    fail("${name} has SHA-256 ${got}, expected ${want}")
  endif()
endforeach()

# ---- 6. a bad squeezed file ----
flip("${work}/sq/raw/EX15.AQM" "${work}/sq/bad/EX15.AQM" 5000)
run("${unsq}" "${work}/sq/bad/EX15.AQM")
if(NOT rc EQUAL 1)
  fail("unsqueeze of a bad file exited ${rc}, expected 1")
endif()
if(NOT err MATCHES "unsqueeze: .*EX15.AQM: the (checksum is wrong|tree is corrupt|file ends before the end code)")
  fail("unsqueeze did not report the bad file")
endif()
if(EXISTS "${work}/sq/bad/EX15.ASM")
  fail("unsqueeze wrote EX15.ASM from a bad file")
endif()

message(STATUS "lbr-tools: PASS")
