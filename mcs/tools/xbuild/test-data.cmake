# The tree the xbuild suites run against.
#
# MSBuildExtensionsPath and XBUILD_FRAMEWORK_FOLDERS_PATH point xbuild at this
# directory.  TESTING_MONO makes ToolLocationHelper look for frameworks and
# build tools in this build's profile directories rather than in an installed
# framework.  Everything here is a copy of what gets installed, staged where an
# uninstalled xbuild can reach it.
mono_profile_dir(_net4x net_4_x)
set(_data "${MONO_MCS_TOPDIR}/tools/xbuild/data")
set(_testing "${_net4x}/tests/xbuild")

# What the suites read to find the tree below.  Set before the guard: every
# includer needs the variable, only the first needs the rules.
set(MONO_XBUILD_TEST_ENV
    "TESTING_MONO=a"
    "MSBuildExtensionsPath=${_testing}/extensions"
    "XBUILD_FRAMEWORK_FOLDERS_PATH=${_testing}/frameworks")

if(TARGET mcs-xbuild-test-data)
  return()
endif()

set(_stamp "${MONO_MANAGED_DEPSDIR}/xbuild-test-data.stamp")

# The targets live beside the assemblies rather than in the staging tree: that
# is where xbuild looks for the ones matching its own version.
set(_copies "")
foreach(_v 4.0 12.0 14.0)
  if(_v STREQUAL 4.0)
    set(_dest "${_net4x}")
  else()
    string(REPLACE "." "_" _p "${_v}")
    mono_profile_dir(_dest xbuild_${_p})
    string(REGEX REPLACE "_0$" "" _dest "${_dest}")
  endif()
  foreach(_f Microsoft.Common.targets Microsoft.Common.tasks Microsoft.CSharp.targets)
    list(APPEND _copies COMMAND "${CMAKE_COMMAND}" -E copy_if_different
         "${_data}/${_v}/${_f}" "${_dest}/${_f}")
  endforeach()
  list(APPEND _copies COMMAND "${CMAKE_COMMAND}" -E copy_if_different
       "${_data}/Microsoft.VisualBasic.targets" "${_dest}/Microsoft.VisualBasic.targets")
endforeach()

add_custom_command(
  OUTPUT "${_stamp}"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${_data}" "${_testing}/extensions"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${MONO_MCS_TOPDIR}/class/Microsoft.Build/xbuild-testing" "${_testing}/frameworks"
  ${_copies}
  COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
  COMMENT "COPY xbuild test data"
  VERBATIM)
add_custom_target(mcs-xbuild-test-data DEPENDS "${_stamp}")
