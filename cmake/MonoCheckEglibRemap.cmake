# Fails if the runtime still exports a bare g_* symbol.
#
# eglib's names are remapped to monoeg_* through eglib-remap.h so that a
# runtime embedded in a host that already links glib does not collide with it.
# A symbol that slipped through the remap only shows up as a mysterious crash
# in an embedder, so it is worth a test.
#
# Run as a script (cmake -P) with NM and BINARY.

execute_process(COMMAND "${NM}" "${BINARY}"
                OUTPUT_VARIABLE _syms
                RESULT_VARIABLE _rc
                ERROR_QUIET)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "nm failed on ${BINARY}")
endif()

# These genuinely belong to mono's own API and are expected to be exported
# under a g_ name.
set(_allowed "g_s?list_(pre|ap)pend_(image|mempool)|g_concat_dir_and_file|g_Ctoc")

# KNOWN GAP, not an exemption on the merits: these two are eglib functions
# (ghashtable.c and goutput.c, declared in glib.h) that are missing from
# eglib-remap.h, so the runtime exports them under their bare names. They
# predate this check -- the automake version only looked at *local* text
# symbols, so it never saw an exported one. Add them to eglib-remap.h
# and delete these two lines.
set(_known_gaps "g_hash_table_lookup_oop|g_log_disabled")

set(_bad "")
string(REPLACE "\n" ";" _lines "${_syms}")
foreach(_l IN LISTS _lines)
  # A defined text symbol whose name starts with g_ (or _g_ where the platform
  # prepends an underscore).
  if(_l MATCHES "[ \t][tT][ \t]+_?(g_[A-Za-z0-9_]+)$")
    set(_name "${CMAKE_MATCH_1}")
    if(NOT _name MATCHES "^(${_allowed})$" AND NOT _name MATCHES "^(${_known_gaps})$")
      list(APPEND _bad "${_name}")
    endif()
  endif()
endforeach()

if(_bad)
  list(REMOVE_DUPLICATES _bad)
  string(REPLACE ";" "\n  " _pretty "${_bad}")
  message(FATAL_ERROR
    "these eglib symbols are exported unremapped -- add them to eglib-remap.h:\n  ${_pretty}")
endif()
