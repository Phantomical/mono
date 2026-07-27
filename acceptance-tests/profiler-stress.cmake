# The profiler stress test, from profiler-stress.mk.
#
# runner.exe drives a set of benchmarks from the `benchmarker` checkout with
# the log profiler attached under every option in turn, looking for crashes.
# It is built from profiler-stress/runner.exe.sources, which is runner.cs plus
# the whole of external/Newtonsoft.Json -- a real git submodule of this
# repository, unlike the acceptance-test checkouts.
#
# runner.cs looks for its benchmark descriptions at ../external/benchmarker
# relative to its working directory, which is why the test runs from
# acceptance-tests' binary directory and why the checkout has to be there.

if(NOT MONO_SUBMODULE_BENCHMARKER_PRESENT)
  return()
endif()
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/Newtonsoft.Json/Src")
  message(STATUS
    "profiler-stress: external/Newtonsoft.Json is not checked out "
    "(git submodule update --init external/Newtonsoft.Json); skipping.")
  return()
endif()

set(_ps_src "${_src}/profiler-stress")

# SYS_REFS from profiler-stress.mk.
set(_ps_refs
  -r:System.dll -r:System.Core.dll -r:System.Data.dll
  -r:System.Runtime.Serialization.dll -r:System.Xml.dll
  -r:System.Xml.Linq.dll -r:Mono.Posix.dll)

# The .sources file lists paths relative to profiler-stress/.
file(STRINGS "${_ps_src}/runner.exe.sources" _ps_rel)
set(_ps_sources "")
foreach(_s IN LISTS _ps_rel)
  string(STRIP "${_s}" _s)
  if(_s)
    get_filename_component(_abs "${_ps_src}/${_s}" ABSOLUTE)
    list(APPEND _ps_sources "${_abs}")
  endif()
endforeach()

# -define:ARCH_$(arch_target); this port is amd64-only.
add_custom_command(
  OUTPUT "${_bin}/runner.exe"
  COMMAND ${_mcs} -debug -define:ARCH_amd64 -target:exe ${_ps_refs}
          "-out:${_bin}/runner.exe" ${_ps_sources}
  DEPENDS ${_ps_sources} acceptance-toolchain
  COMMENT "CSC runner.exe (profiler-stress)"
  VERBATIM)
add_custom_target(profiler-stress-runner DEPENDS "${_bin}/runner.exe")

# runner.cs hardcodes three paths relative to its working directory:
#   ../external/benchmarker        the benchmark descriptions
#   ../../runtime/mono-wrapper     the runtime under test
#   ../../mcs/class/lib/net_4_x    the class libraries
# automake ran it from <srcdir>/acceptance-tests/profiler-stress, where all
# three resolved because the build was in-tree.  Running it from the matching
# directory in the build tree gets the last two right on its own -- and gets
# the *built* runtime rather than the source tree's -- so only external/ has
# to be bridged.
file(MAKE_DIRECTORY "${_bin}/profiler-stress")
if(NOT EXISTS "${_bin}/external")
  file(CREATE_LINK "${_ext}" "${_bin}/external" SYMBOLIC)
endif()
if(NOT EXISTS "${CMAKE_BINARY_DIR}/mcs")
  file(CREATE_LINK "${MONO_MCS_TOPDIR}" "${CMAKE_BINARY_DIR}/mcs" SYMBOLIC)
endif()

add_test(NAME profiler-stress-build
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                 --target profiler-stress-runner)
set_tests_properties(profiler-stress-build PROPERTIES
  FIXTURES_SETUP profiler_stress LABELS "acceptance;fixture" TIMEOUT 1800)

# runner.cs gives each benchmark a six-hour ceiling of its own and walks every
# profiler option in turn, so the whole run is measured in hours.
add_test(NAME profiler-stress
         COMMAND ${_runtime} "${_bin}/runner.exe"
         WORKING_DIRECTORY "${_bin}/profiler-stress")
set_tests_properties(profiler-stress PROPERTIES
  LABELS "acceptance;stress"
  FIXTURES_REQUIRED profiler_stress
  TIMEOUT 21600)
