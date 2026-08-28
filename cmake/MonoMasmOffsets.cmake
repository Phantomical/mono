# Turns a C header of `#define NAME value` lines into the MASM `NAME EQU value`
# the assembler reads.
#
# GAS runs a source through the C preprocessor, so a .S includes such a header
# directly.  ml64 has no preprocessor step of its own, so without this the
# numbers would have to be written a second time in the .asm, and the two
# copies would disagree the first time a layout moved.
#
# Run as a script:
#   cmake -D IN=<header> -D OUT=<inc> -D DEFINES=<a;b> -P MonoMasmOffsets.cmake
#
# DEFINES names the macros the header's own #ifdefs are read under, which is
# what selects the host's arm of a layout that has one per host.

if(NOT DEFINED IN OR NOT DEFINED OUT)
  message(FATAL_ERROR "MonoMasmOffsets.cmake: IN and OUT are required")
endif()

file(STRINGS "${IN}" _lines)

# One nesting level of #ifdef/#else/#endif, which is all a layout header uses.
# `taking` is whether the arm now being read is the one this host gets.
set(_taking TRUE)
set(_depth 0)
set(_out "; Generated from ${IN} by cmake/MonoMasmOffsets.cmake.  Do not edit.\n")

foreach(_line IN LISTS _lines)
  if(_line MATCHES "^[ \t]*#[ \t]*ifdef[ \t]+([A-Za-z_][A-Za-z0-9_]*)")
    math(EXPR _depth "${_depth} + 1")
    if("${CMAKE_MATCH_1}" IN_LIST DEFINES)
      set(_taking TRUE)
    else()
      set(_taking FALSE)
    endif()
  elseif(_line MATCHES "^[ \t]*#[ \t]*else" AND _depth GREATER 0)
    if(_taking)
      set(_taking FALSE)
    else()
      set(_taking TRUE)
    endif()
  elseif(_line MATCHES "^[ \t]*#[ \t]*endif" AND _depth GREATER 0)
    math(EXPR _depth "${_depth} - 1")
    set(_taking TRUE)
  elseif(_taking AND _line MATCHES "^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]+(0x[0-9a-fA-F]+|[0-9]+)")
    # MASM spells a hexadecimal literal with a trailing h and a leading digit.
    set(_name "${CMAKE_MATCH_1}")
    set(_value "${CMAKE_MATCH_2}")
    if(_value MATCHES "^0x(.*)$")
      set(_value "0${CMAKE_MATCH_1}h")
    endif()
    string(APPEND _out "${_name} EQU ${_value}\n")
  endif()
endforeach()

# Only rewrite on a change, so a build that regenerates this does not reassemble
# every source that includes it.
set(_old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _out)
  file(WRITE "${OUT}" "${_out}")
endif()
