# Compiles one managed assembly.  Run as `cmake -D SETTINGS=<file> -P`.
#
# This is a script, not a module: it is the body of the single custom command
# behind every .dll and .exe under mcs/.  Everything it needs is written into
# SETTINGS at configure time by _mono_materialize_one(), so the command line
# stays short and lists survive without quoting games.
#
# The reason a compile is a script rather than a plain COMMAND is the depfile.
# csc cannot emit one, and CMake cannot know the sources at configure time
# because they come out of gensources.  So the same step that expands the
# source list also writes `<assembly>: <every .cs>` next to it, and ninja picks
# that up after the command runs.  Editing a .cs then rebuilds exactly the
# assemblies that name it.

cmake_minimum_required(VERSION 3.28)

if(NOT SETTINGS)
  message(FATAL_ERROR "MonoCompileAssembly.cmake: -D SETTINGS=<file> is required")
endif()
include("${SETTINGS}")

# Every tool below runs from the assembly's source directory: gensources emits
# paths relative to it, and the response file it writes is therefore only
# meaningful with that as the working directory.
set(_wd "${MCS_SOURCE_DIR}")

function(_mono_run what)
  cmake_parse_arguments(ARG "" "" "COMMAND;ENVIRONMENT" ${ARGN})
  set(_cmd ${ARG_COMMAND})
  if(ARG_ENVIRONMENT)
    set(_cmd "${CMAKE_COMMAND}" -E env ${ARG_ENVIRONMENT} ${_cmd})
  endif()
  execute_process(COMMAND ${_cmd} WORKING_DIRECTORY "${_wd}" RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    # The command has already written its own diagnostics to stderr. Repeating
    # the argv here is what tells you *which* assembly out of ~900 failed.
    message(FATAL_ERROR "${what} failed (${_rc}) for ${MCS_OUTPUT}")
  endif()
endfunction()

# 1. The source list
#
# Libraries go through gensources, which resolves the `#include` directives and
# the platform/profile fallback chain across the directory's *.sources files.
# Programs do not: on Linux their .sources file is already a flat list and the
# compiler is handed it verbatim, which is also how gensources itself is able
# to bootstrap before any gensources exists.
if(MCS_GENSOURCES)
  file(REMOVE "${MCS_RESPONSE}")
  set(_gs ${MCS_GENSOURCES})
  if(MCS_TOOL_ENV)
    set(_gs "${CMAKE_COMMAND}" -E env ${MCS_TOOL_ENV} ${_gs})
  endif()
  string(JOIN "," _gs_platforms ${MCS_PLATFORM_NAMES})
  string(JOIN "," _gs_profiles ${MCS_PROFILE_NAMES})
  # Spelled out rather than routed through _mono_run because the platform
  # argument is empty for the xbuild profiles and gensources reads its four
  # trailing arguments positionally.  An empty element inside an unquoted list
  # expansion disappears, so through _mono_run the empty platform would go away
  # and the profile would land in its slot, with no error.
  execute_process(
    COMMAND ${_gs} --strict
            "--platforms:${_gs_platforms}" "--profiles:${_gs_profiles}"
            ${MCS_GENSOURCES_BASEDIR}
            "${MCS_RESPONSE}" "${MCS_LIBRARY}" "${MCS_PLATFORM}" "${MCS_PROFILE}"
    WORKING_DIRECTORY "${_wd}" RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "gensources failed (${_rc}) for ${MCS_OUTPUT}")
  endif()
  # --strict signals some failures by deleting its output rather than by
  # returning non-zero.
  if(NOT EXISTS "${MCS_RESPONSE}")
    message(FATAL_ERROR "gensources produced no source list for ${MCS_OUTPUT}")
  endif()
endif()

# 2. The depfile
file(STRINGS "${MCS_RESPONSE}" _srcs ENCODING UTF-8)

set(_deps "")
foreach(_s IN LISTS _srcs)
  string(STRIP "${_s}" _s)
  if(_s STREQUAL "")
    continue()
  endif()
  if(NOT IS_ABSOLUTE "${_s}")
    set(_s "${_wd}/${_s}")
  endif()
  # A program's .sources file is handed to csc verbatim and csc expands
  # wildcards itself, so an entry can be `.../reflect/*.cs`.  A depfile cannot
  # carry a pattern -- ninja would treat it as a filename that never appears --
  # so it is expanded here into what it currently matches.
  if(_s MATCHES "[*?]")
    file(GLOB _matches "${_s}")
    list(APPEND _deps ${_matches})
    continue()
  endif()
  # Ninja splits a depfile path at a backtick whatever the escaping, and would
  # then wait forever on two inputs that do not exist.  MonoManaged.cmake makes
  # these configure-time dependencies instead.
  if(_s MATCHES "`")
    continue()
  endif()
  list(APPEND _deps "${_s}")
endforeach()

# The .sources files themselves belong in the depfile too: adding one to a
# directory changes the answer without changing any file already listed.
list(APPEND _deps ${MCS_SOURCES_INPUTS})

# GNU depfile syntax -- a space separates dependencies, so any space inside a
# path has to be escaped.  Backslashes are not path separators here.
set(_lines "")
foreach(_d IN LISTS _deps)
  string(REPLACE " " "\\ " _d "${_d}")
  string(APPEND _lines " \\\n  ${_d}")
endforeach()
string(REPLACE " " "\\ " _target "${MCS_OUTPUT}")
file(WRITE "${MCS_DEPFILE}" "${_target}:${_lines}\n")

# 3. Compile
get_filename_component(_outdir "${MCS_OUTPUT}" DIRECTORY)
get_filename_component(_builddir "${MCS_BUILD_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_outdir}" "${_builddir}")

# csc rewrites its outputs whatever they held before, so the reference assembly
# goes to a scratch name.  The copy consumers depend on is published below.
set(_refout "")
if(MCS_REFOUT)
  get_filename_component(_refdir "${MCS_REFOUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_refdir}")
  set(_refout "-refout:${MCS_REFOUT}.tmp")
endif()

_mono_run("csc"
  COMMAND ${MCS_CSC} ${MCS_CSC_FLAGS} "-out:${MCS_BUILD_OUTPUT}" ${_refout}
          ${MCS_BUILT_SOURCES} "@${MCS_RESPONSE}"
  ENVIRONMENT ${MCS_CSC_ENV})

# The unchanged mtime this leaves behind is what keeps a consumer clean.  The
# REFERENCE_ASSEMBLY block in MonoManaged.cmake has the rest of the mechanism
# and what the file describes.
if(MCS_REFOUT)
  file(COPY_FILE "${MCS_REFOUT}.tmp" "${MCS_REFOUT}" ONLY_IF_DIFFERENT)
endif()

# 4. Post-processing
#
# corlib is the only assembly that gets here with real work to do: its string
# table and the ilasm'd Unsafe.il module are spliced into the freshly built
# mscorlib.dll, so what ships is rewritten IL rather than what csc emitted.
if(MCS_STRING_REPLACER)
  _mono_run("cil-stringreplacer"
    COMMAND ${MCS_STRING_REPLACER} ${MCS_STRING_REPLACER_FLAGS} "${MCS_BUILD_OUTPUT}"
    ENVIRONMENT ${MCS_TOOL_ENV})
endif()

if(MCS_SN)
  _mono_run("sn"
    COMMAND ${MCS_SN} -R "${MCS_BUILD_OUTPUT}" "${MCS_SNK}"
    ENVIRONMENT ${MCS_TOOL_ENV})
endif()

# A handful of bootstrap assemblies compile into an isolated `tmp/` directory
# and are copied up afterwards.  See the INTERMEDIATE note in MonoManaged.cmake
# for why that directory exists at all.
if(NOT MCS_BUILD_OUTPUT STREQUAL MCS_OUTPUT)
  file(COPY_FILE "${MCS_BUILD_OUTPUT}" "${MCS_OUTPUT}")
  # .mdb hangs off the full name, .pdb replaces the extension.
  if(EXISTS "${MCS_BUILD_OUTPUT}.mdb")
    file(RENAME "${MCS_BUILD_OUTPUT}.mdb" "${MCS_OUTPUT}.mdb")
  endif()
  string(REGEX REPLACE "\\.(dll|exe)$" ".pdb" _from_pdb "${MCS_BUILD_OUTPUT}")
  string(REGEX REPLACE "\\.(dll|exe)$" ".pdb" _to_pdb   "${MCS_OUTPUT}")
  if(EXISTS "${_from_pdb}")
    file(RENAME "${_from_pdb}" "${_to_pdb}")
  endif()
endif()
