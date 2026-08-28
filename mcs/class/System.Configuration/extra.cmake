# The standalone configuration tests: 44 programs, each with an app.config
# beside it, each printing what the configuration system made of it.
#
# The Makefile's own `run-test` rule replaced the NUnit suite with these, but it
# invoked `make -C Test/standalone` with no goal, so it only ever compiled them
# -- `check`, which runs and diffs, was never reached.  Here they run.
#
# The assertion is the exact stdout, so each case gets its own staged directory:
# several rewrite their own config file, and ConfigurationManager keys off the
# executable's path.
set(_src "${CMAKE_CURRENT_SOURCE_DIR}/Test/standalone")
set(_bin "${CMAKE_CURRENT_BINARY_DIR}/standalone")
mono_profile_dir(_pdir net_4_x)

# t13, t14, t26 and t27 are commented out of the Makefile's TESTS.
set(_cases "")
foreach(_n RANGE 1 48)
  if(NOT _n MATCHES "^(13|14|26|27)$")
    list(APPEND _cases t${_n})
  endif()
endforeach()

_mono_csc_command(_csc net_4_x)
_mono_csc_env(_cscenv net_4_x)
_mono_tool_depends(_rt net_4_x)
set(_cscmd ${_csc})
if(_cscenv)
  set(_cscmd "${CMAKE_COMMAND}" -E env ${_cscenv} ${_csc})
endif()

set(_refflags -nostdlib "-r:${_pdir}/mscorlib.dll")
set(_refdeps mcs-net_4_x-corlib)
foreach(_r System.Configuration System.Web System.Data System System.Xml)
  list(APPEND _refflags "-r:${_pdir}/${_r}.dll")
  list(APPEND _refdeps "mcs-net_4_x-${_r}")
endforeach()

set(_exes "")
foreach(_t IN LISTS _cases)
  set(_run "${_bin}/${_t}")
  set(_exe "${_run}/${_t}.exe")

  # t36 and t46 read their section handler out of a second assembly, so the
  # configuration system has to bind a type by name from beside the program.
  set(_libflags "")
  set(_libdeps "")
  set(_extra "")
  if(EXISTS "${_src}/${_t}-lib.cs")
    add_custom_command(
      OUTPUT "${_run}/${_t}-lib.dll"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_run}"
      COMMAND ${_cscmd} -nologo -noconfig -t:library ${_refflags}
              "-out:${_run}/${_t}-lib.dll" "${_src}/${_t}-lib.cs"
      DEPENDS "${_src}/${_t}-lib.cs" ${_refdeps} ${_rt}
      COMMENT "CSC [net_4_x] standalone/${_t}-lib.dll"
      VERBATIM)
    set(_libflags "-r:${_run}/${_t}-lib.dll")
    set(_libdeps "${_run}/${_t}-lib.dll")
  endif()

  # Assert.cs is the whole test framework these use.  t36 is the one case that
  # does its printing itself.
  set(_srcs "${_src}/${_t}.cs")
  if(NOT _t STREQUAL "t36")
    list(APPEND _srcs "${_src}/Assert.cs")
  endif()

  # Each config file is copied rather than symlinked: the cases that call
  # Configuration.Save write back over it.
  foreach(_c "${_t}.exe.config" "${_t}.exe.config2")
    if(EXISTS "${_src}/${_c}")
      list(APPEND _extra COMMAND "${CMAKE_COMMAND}" -E copy_if_different
           "${_src}/${_c}" "${_run}/${_c}")
    endif()
  endforeach()

  add_custom_command(
    OUTPUT "${_exe}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_run}"
    ${_extra}
    COMMAND ${_cscmd} -nologo -noconfig ${_refflags} ${_libflags}
            "-out:${_exe}" ${_srcs}
    DEPENDS ${_srcs} ${_libdeps} ${_refdeps} ${_rt}
    COMMENT "CSC [net_4_x] standalone/${_t}.exe"
    VERBATIM)
  list(APPEND _exes "${_exe}")

  add_test(NAME config-standalone-${_t}
           COMMAND "${CMAKE_COMMAND}"
                   -D "RUNTIME=${MONO_RUNTIME_WRAPPER}"
                   -D "PROFILE_DIR=${_pdir}"
                   -D "RUN_DIR=${_run}"
                   -D "EXE=${_exe}"
                   -D "EXPECTED=${_src}/${_t}.exe.expected"
                   -P "${_src}/standalone-test.cmake")
  set_tests_properties(config-standalone-${_t} PROPERTIES
    LABELS bcl TIMEOUT 300
    ENVIRONMENT "MONO_PATH=${_pdir}")

  # t12 gives a ConfigurationProperty a DefaultValue of the wrong type and
  # expects the plain System.Exception .NET reports.  This System.Configuration
  # lets the ArgumentException from the conversion out instead.  Nothing was
  # comparing this output before, so the divergence is as old as the case.
  if(_t STREQUAL "t12")
    set_tests_properties(config-standalone-${_t} PROPERTIES WILL_FAIL TRUE)
  endif()
endforeach()

add_custom_target(mcs-config-standalone-tests ALL DEPENDS ${_exes})
