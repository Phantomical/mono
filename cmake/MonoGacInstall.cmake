# Installs one assembly into the GAC, at install time.  Included from an
# install(CODE) block, which sets the MONO_GAC_* variables below.
#
# gacutil rather than a set of install(FILES) rules because it is the thing
# that knows the layout: lib/mono/gac/<Name>/<version>_<culture>_<token>/ needs
# the assembly's version and public key token, and the package directory gets a
# *relative* symlink back into it.  Reimplementing that here would mean parsing
# assembly metadata in CMake for no gain.
#
#   MONO_GAC_RUNTIME    the mono to run it on
#   MONO_GAC_TOOL       gacutil.exe
#   MONO_GAC_MONO_PATH  where its own dependencies live
#   MONO_GAC_LIBDIR     the install prefix's libdir, DESTDIR not applied yet
#   MONO_GAC_LIB        the assembly to install
#   MONO_GAC_PACKAGE    the package directory, or empty for a GAC-only install

set(_root "$ENV{DESTDIR}${MONO_GAC_LIBDIR}")

set(_args /i "${MONO_GAC_LIB}" /f /root "${_root}")
if(MONO_GAC_PACKAGE)
  list(APPEND _args /package "${MONO_GAC_PACKAGE}")
endif()

# No /gacdir: this runtime resolves the GAC consistently from /root.
message(STATUS "GAC install: ${MONO_GAC_LIB}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${MONO_GAC_MONO_PATH}"
          "${MONO_GAC_RUNTIME}" "${MONO_GAC_TOOL}" ${_args}
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "gacutil failed (${_rc}) for ${MONO_GAC_LIB}")
endif()
