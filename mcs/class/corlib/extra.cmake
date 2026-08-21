# Everything the corlib test suites need beside the two test assemblies.
#
# Three pieces: the satellite resource assemblies the resource-manager tests
# look up by culture, two helper assemblies the xunit suite loads by name, and
# the version-tolerant-serialization corpus, which is a suite of its own.
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

set(_corlib_test_deps "")

# Satellite assemblies
#
# ResourceManager finds these by walking <culture>/ next to the assembly that
# asked, so they have to sit beside the test assembly under its own culture
# directory -- the layout is the assertion.
_mono_tool_command(_resgen net_4_x resgen.exe)
_mono_tool_env(_resgen_env net_4_x)
get_property(_rgprov GLOBAL PROPERTY MONO_MANAGED_PROVIDER_build/resgen)

foreach(_culture es-ES nn-NO)
  set(_res "${CMAKE_CURRENT_BINARY_DIR}/Resources.${_culture}.resources")
  add_custom_command(
    OUTPUT "${_res}"
    COMMAND "${CMAKE_COMMAND}"
            -D "RESGEN=${_resgen}" -D "RESGEN_ENV=${_resgen_env}"
            -D "INPUT=${_src}/Test/resources/Resources.${_culture}.resx"
            -D "OUTPUT=${_res}"
            -D "PREBUILT=${_src}/Test/resources/Resources.${_culture}.resources.prebuilt"
            -D "WORKDIR=${_src}"
            -P "${CMAKE_SOURCE_DIR}/cmake/MonoResgen.cmake"
    DEPENDS "${_src}/Test/resources/Resources.${_culture}.resx" ${_rt} ${_rgprov}
    COMMENT "RESGEN [net_4_x] Resources.${_culture}.resources"
    VERBATIM)

  set(_sat "${_tests}/${_culture}/net_4_x_corlib_test.resources.dll")
  add_custom_command(
    OUTPUT "${_sat}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_tests}/${_culture}"
    COMMAND ${_cscmd} ${MONO_MANAGED_COMMON_FLAGS} -nostdlib -target:library
            -warn:0 "-r:${_pdir}/mscorlib.dll"
            "-resource:${_res},Resources.${_culture}.resources"
            "${_src}/Test/resources/culture-${_culture}.cs" "-out:${_sat}"
    DEPENDS "${_src}/Test/resources/culture-${_culture}.cs" "${_res}"
            mcs-net_4_x-corlib ${_rt}
    COMMENT "CSC [net_4_x] ${_culture}/net_4_x_corlib_test.resources.dll"
    VERBATIM)
  add_custom_target(mcs-corlib-satellite-${_culture} DEPENDS "${_sat}")
  list(APPEND _corlib_test_deps mcs-corlib-satellite-${_culture})
endforeach()

# The xunit suite's helper assemblies
#
# TestLoadAssembly.dll is loaded from beside the test assembly by name rather
# than referenced, which is the point of the tests that use it.
# System.Reflection.TestModule.dll is both referenced at compile time and
# loaded from beside the assembly by name at runtime.
set(_corefx "${CMAKE_SOURCE_DIR}/external/corefx/src/System.Runtime/tests")

mono_test_fixture_assembly(
  PROFILE  net_4_x
  ASSEMBLY mscorlib.dll
  NAME     TestLoadAssembly.dll
  IN_TESTS_DIR
  SOURCES  "${_corefx}/TestLoadAssembly/TestLoadAssembly.cs")

set(_module "${_tests}/System.Reflection.TestModule.dll")
add_custom_command(
  OUTPUT "${_module}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_tests}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${_corefx}/TestModule/System.Reflection.TestModule.dll" "${_module}"
  DEPENDS "${_corefx}/TestModule/System.Reflection.TestModule.dll"
  VERBATIM)
add_custom_target(mcs-corlib-test-module DEPENDS "${_module}")

mono_test_environment(PROFILE net_4_x ASSEMBLY mscorlib.dll
                      DEPENDS ${_corlib_test_deps} mcs-corlib-test-module)

# Version-tolerant serialization
#
# Six versions of the same type, each in its own assembly: the suite serializes
# with one and deserializes with another, so the assemblies are the fixture and
# the whole thing is a separate test rather than part of the corlib suite.
set(_vts_src "${_src}/Test/System.Runtime.Serialization.Formatters.Binary/VersionTolerantSerialization")
set(_vts "${_tests}/vts")
set(_vts_libs "")

foreach(_v 1.0 2.0 3.0 4.0 5.0 6.0)
  set(_lib "${_vts}/${_v}/Address.dll")
  add_custom_command(
    OUTPUT "${_lib}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_vts}/${_v}"
    COMMAND ${_cscmd} ${MONO_MANAGED_COMMON_FLAGS} -nostdlib -target:library
            -warn:0 "-r:${_pdir}/mscorlib.dll" "-out:${_lib}"
            "${_vts_src}/VersionTolerantSerializationTestLib/${_v}/Address.cs"
    DEPENDS "${_vts_src}/VersionTolerantSerializationTestLib/${_v}/Address.cs"
            mcs-net_4_x-corlib ${_rt}
    COMMENT "CSC [net_4_x] vts/${_v}/Address.dll"
    VERBATIM)
  list(APPEND _vts_libs "${_lib}")
endforeach()

set(_vts_test "${_tests}/BinarySerializationOverVersionsTest.dll")
add_custom_command(
  OUTPUT "${_vts_test}"
  COMMAND ${_cscmd} ${MONO_MANAGED_COMMON_FLAGS} -nostdlib -target:library
          -warn:0 "-r:${_pdir}/mscorlib.dll" "-r:${_pdir}/System.dll"
          "-r:${_pdir}/nunitlite.dll" "-r:${_vts}/1.0/Address.dll"
          "${_vts_src}/BinarySerializationOverVersionsTest.cs" "-out:${_vts_test}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${MONO_MCS_TOPDIR}/tools/nunit-lite/nunit-lite-console/nunit-lite-console.exe.config.tmpl"
          "${_vts_test}.nunitlite.config"
  DEPENDS "${_vts_src}/BinarySerializationOverVersionsTest.cs" ${_vts_libs}
          mcs-net_4_x-corlib mcs-net_4_x-System mcs-net_4_x-nunitlite ${_rt}
  COMMENT "CSC [net_4_x] BinarySerializationOverVersionsTest.dll"
  VERBATIM)
add_custom_target(mcs-corlib-vts-tests ALL DEPENDS "${_vts_test}")
add_dependencies(mcs-corlib-vts-tests mcs-net_4_x-nunit-lite-console)

add_test(NAME bcl-corlib-vts
         COMMAND "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" --debug
                 "${_pdir}/nunit-lite-console.exe" "${_vts_test}"
                 -format:nunit2
                 "-result:${_tests}/TestResult-net_4_x-corlib-vts.xml"
         WORKING_DIRECTORY "${_tests}")
set_tests_properties(bcl-corlib-vts PROPERTIES
  LABELS bcl TIMEOUT 1800
  ENVIRONMENT "MONO_PATH=${_pdir}:${_tests};MONO_TESTS_IN_PROGRESS=yes")
