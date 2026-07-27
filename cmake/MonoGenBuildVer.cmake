# Writes mono/mini/buildver-<gc>.h, which carries the build date the runtime
# prints in --version.  Run as a script (cmake -P) with OUT set.
#
# SOURCE_DATE_EPOCH is honoured so reproducible builds stay reproducible.

if(DEFINED ENV{SOURCE_DATE_EPOCH} AND NOT "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
  execute_process(COMMAND date -u -d "@$ENV{SOURCE_DATE_EPOCH}"
                  OUTPUT_VARIABLE _date OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
endif()
if(NOT _date)
  string(TIMESTAMP _date "%a %b %d %H:%M:%S UTC %Y" UTC)
endif()

file(WRITE "${OUT}" "const char *build_date = \"${_date}\";\n")
