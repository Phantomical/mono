# The symbolicate round-trip check. Only the JIT variant is registered: the two
# AOT ones ask for --aot, which this runtime refuses outright.
mono_profile_dir(_pdir net_4_x)
set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_exe "${CMAKE_CURRENT_BINARY_DIR}/StackTraceDumper.exe")

_mono_csc_command(_csc net_4_x)
_mono_csc_env(_cscenv net_4_x)
_mono_tool_depends(_rt net_4_x)
set(_cscmd ${_csc})
if(_cscenv)
  set(_cscmd "${CMAKE_COMMAND}" -E env ${_cscenv} ${_csc})
endif()

add_custom_command(
  OUTPUT "${_exe}"
  COMMAND ${_cscmd} /nologo /noconfig -nostdlib ${MONO_MANAGED_DEBUG_FLAGS}
          -warn:0 "-r:${_pdir}/mscorlib.dll" "-r:${_pdir}/System.Core.dll"
          "-out:${_exe}" "${_src}/Test/StackTraceDumper.cs"
  DEPENDS "${_src}/Test/StackTraceDumper.cs" mcs-net_4_x-corlib
          mcs-net_4_x-System.Core ${_rt}
  COMMENT "CSC [net_4_x] StackTraceDumper.exe"
  VERBATIM)
add_custom_target(mcs-symbolicate-tests ALL DEPENDS "${_exe}")
add_dependencies(mcs-symbolicate-tests mcs-net_4_x-mono-symbolicate)

add_test(NAME symbolicate-without_aot
         COMMAND "${CMAKE_COMMAND}"
                 -D "RUNTIME=${CMAKE_BINARY_DIR}/runtime/mono-wrapper"
                 -D "LIB_PATH=${_pdir}"
                 -D "PROGRAM=${_pdir}/mono-symbolicate.exe"
                 -D "TEST_EXE=${_exe}"
                 -D "OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/without_aot"
                 -D "EXPECTED=${_src}/Test/symbolicate.expected"
                 -D "AOT="
                 -P "${_src}/symbolicate-test.cmake")
set_tests_properties(symbolicate-without_aot PROPERTIES
  LABELS "tools" TIMEOUT 900)
