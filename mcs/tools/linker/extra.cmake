# The linker's regression suite: compile a small program, link the class
# libraries down to what it reaches, and run it.
#
# Each case gets its own output directory, so unlike the Makefile -- which had
# to declare .NOTPARALLEL because every case wrote the same illink-output --
# these run concurrently.
mono_profile_dir(_pdir net_4_x)
mono_profile_dir(_build build)
set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin "${CMAKE_CURRENT_BINARY_DIR}")

# Directory name -> the extra reference programs in it compile against.
set(_dir_mscorlib_refs "")
set(_dir_System_refs System)
set(_dir_System.Core_refs System.Core)

set(_cases
    mscorlib/test-array
    mscorlib/test-calendar-01
    mscorlib/test-calendar-02
    mscorlib/test-exception-01
    mscorlib/test-locale-01
    mscorlib/test-methodimpl-01
    mscorlib/test-reflection-01
    mscorlib/test-reflection-02
    mscorlib/test-reflection-03
    mscorlib/test-reflection-04
    mscorlib/test-string-01
    mscorlib/test-string-02
    mscorlib/test-string-03
    mscorlib/test-task-01
    mscorlib/test-remoting
    mscorlib/test-reflection
    System/test-security
    System/test-typeconverter
    System.Core/test-plinq-01
    System.Core/test-queryable-01
    System.Core/test-queryable-02)

_mono_csc_command(_csc net_4_x)
_mono_csc_env(_cscenv net_4_x)
_mono_tool_depends(_rt net_4_x)
set(_cscmd ${_csc})
if(_cscenv)
  set(_cscmd "${CMAKE_COMMAND}" -E env ${_cscenv} ${_csc})
endif()

set(_exes "")
foreach(_case IN LISTS _cases)
  get_filename_component(_dir "${_case}" DIRECTORY)
  get_filename_component(_stem "${_case}" NAME)
  string(REPLACE "." "-" _slug "${_case}")
  string(REPLACE "/" "-" _slug "${_slug}")
  set(_exe "${_bin}/Tests/${_case}.exe")

  set(_refflags "")
  set(_refdeps "")
  foreach(_r IN LISTS _dir_${_dir}_refs)
    list(APPEND _refflags "-r:${_pdir}/${_r}.dll")
    list(APPEND _refdeps "mcs-net_4_x-${_r}")
  endforeach()

  add_custom_command(
    OUTPUT "${_exe}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bin}/Tests/${_dir}"
    COMMAND ${_cscmd} -nologo -noconfig -unsafe -nostdlib -debug:portable
            "-r:${_pdir}/mscorlib.dll" ${_refflags}
            "${_src}/Tests/${_case}.cs" "/out:${_exe}"
    DEPENDS "${_src}/Tests/${_case}.cs" mcs-net_4_x-corlib ${_refdeps} ${_rt}
    COMMENT "CSC [net_4_x] linker/${_case}.exe"
    VERBATIM)
  list(APPEND _exes "${_exe}")

  # test-calendar-01 checks that a culture the program never names is still
  # there, so it links with the mideast set rooted rather than nothing.
  set(_roots none)
  if(_stem STREQUAL "test-calendar-01")
    set(_roots mideast)
  endif()

  add_test(NAME linker-${_slug}
           COMMAND "${CMAKE_COMMAND}"
                   -D "RUNTIME=${MONO_RUNTIME_WRAPPER}"
                   -D "TOOLS_PATH=${_build}"
                   -D "LINKER=${_build}/monolinker.exe"
                   -D "PROFILE_DIR=${_pdir}"
                   -D "TEST_EXE=${_exe}"
                   -D "OUT_DIR=${_bin}/illink-output/${_slug}"
                   -D "ROOTS=${_roots}"
                   -P "${_src}/linker-test.cmake")
  set_tests_properties(linker-${_slug} PROPERTIES
    LABELS "tools" TIMEOUT 900)
endforeach()

add_custom_target(mcs-linker-tests ALL DEPENDS ${_exes})
add_dependencies(mcs-linker-tests mcs-build-monolinker)
