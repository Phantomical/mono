# The two programs the suite debugs.  dtest.cs is the debugger: it launches
# one of these under the runtime, sets breakpoints in it and single-steps it.
# They are the subject of every test, not a helper for a few.
#
# They are compiled unoptimized and with a sourcelink map, because the tests
# assert on line numbers, local variables and the source paths for both.
mono_profile_dir(_pdir net_4_x)
set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_tests "${_pdir}/tests")

_mono_csc_command(_csc net_4_x)
_mono_csc_env(_cscenv net_4_x)
_mono_tool_depends(_rt net_4_x)
set(_cscmd ${_csc})
if(_cscenv)
  set(_cscmd "${CMAKE_COMMAND}" -E env ${_cscenv} ${_csc})
endif()

set(_helpers "${_src}/../test-helpers/NetworkHelpers.cs" "${_src}/Test/TypeLoadClass.cs")

add_custom_command(
  OUTPUT "${_tests}/dtest-app.exe"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_tests}"
  COMMAND ${_cscmd} ${MONO_MANAGED_COMMON_FLAGS} -nostdlib
          ${MONO_MANAGED_DEBUG_FLAGS} -unsafe -optimize- -langversion:preview
          "-r:${_pdir}/mscorlib.dll" "-r:${_pdir}/System.dll"
          "-r:${_pdir}/System.Core.dll"
          "-r:${_pdir}/System.Runtime.CompilerServices.Unsafe.dll"
          "-sourcelink:${_src}/Test/sourcelink.json"
          "-out:${_tests}/dtest-app.exe"
          "${_src}/Test/dtest-app.cs" ${_helpers}
  DEPENDS "${_src}/Test/dtest-app.cs" "${_src}/Test/sourcelink.json" ${_helpers}
          mcs-net_4_x-corlib mcs-net_4_x-System mcs-net_4_x-System.Core
          system-runtime-compilerservices-unsafe-net_4_x ${_rt}
  COMMENT "CSC [net_4_x] dtest-app.exe"
  VERBATIM)

mono_add_il_module(
  TARGET  mcs-dtest-excfilter
  OUTPUT  "${_tests}/dtest-excfilter.exe"
  SOURCES "${_src}/Test/dtest-excfilter.il"
  PROFILE net_4_x
  FLAGS   /exe /debug)

add_custom_target(mcs-dtest-app DEPENDS "${_tests}/dtest-app.exe")
# ilasm writes straight to its -out: path, so something has to have made the
# directory first. The rule above is the one that does.
add_dependencies(mcs-dtest-excfilter mcs-dtest-app)

mono_test_environment(PROFILE net_4_x ASSEMBLY Mono.Debugger.Soft.dll
                      DEPENDS mcs-dtest-app mcs-dtest-excfilter)

# Every case launches a debuggee suspended, waiting for the debugger to attach.
# A case that ends without resuming the debuggee to exit leaves it waiting
# forever. About a dozen do, in a run that otherwise passes, so the suite is
# followed by a sweep.
#
# Matched on the full path of the exe this build tree produced, which no other
# process on the machine can be running.
if(WIN32)
  # No pkill here. PowerShell matches on the same full path and is made to
  # exit 1 when it matched nothing, so SKIP_RETURN_CODE below reads either
  # sweep the same way. `?` stands for one character, which lets the pattern
  # match whichever separator the command line was built with.
  string(REPLACE "/" "?" _dtest_glob "${_tests}/dtest-app.exe")
  set(_sweep powershell -NoProfile -NonInteractive -Command
      "$found = @(Get-CimInstance Win32_Process -Filter \"Name='mono-sgen.exe'\" | Where-Object { $_.CommandLine -like '*${_dtest_glob}*' }); if ($found.Count -eq 0) { exit 1 }; $found | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }; exit 0")
else()
  set(_sweep pkill -f "^.*mono-sgen .*${_tests}/dtest-app\\.exe")
endif()

add_test(NAME bcl-Mono.Debugger.Soft-cleanup COMMAND ${_sweep})
set_tests_properties(bcl-Mono.Debugger.Soft-cleanup PROPERTIES
  LABELS fixture
  FIXTURES_CLEANUP fx_bcl-Mono.Debugger.Soft
  # The sweep exits 1 when it matched nothing, which is the good outcome.
  SKIP_RETURN_CODE 1)
