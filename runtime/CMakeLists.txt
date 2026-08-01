# The uninstalled runtime: wrapper scripts and a build-tree copy of the
# configuration, plus the driver for the mcs class-library build.
#
# mcs has its own hand-written make system, which this build invokes rather
# than reimplements: it is not automake, and porting it is a separate job from
# converting the native build.

set(MONO_BUILD_ROOT     "${CMAKE_BINARY_DIR}")
set(MONO_BUILD_CFG_DIR  "${CMAKE_CURRENT_BINARY_DIR}/etc")
set(MONO_RUNTIME_BINARY "${CMAKE_BINARY_DIR}/mono/mini/mono-${MONO_DEFAULT_GC_SUFFIX}")

mono_configure_script("${CMAKE_SOURCE_DIR}/cmake/mono-wrapper.in"
                      "${CMAKE_CURRENT_BINARY_DIR}/mono-wrapper")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/monodis-wrapper"
"#! /bin/sh
MONO_CFG_DIR='${MONO_BUILD_CFG_DIR}'
export MONO_CFG_DIR
exec \"${CMAKE_BINARY_DIR}/mono/dis/monodis\" \"$@\"
")
file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/monodis-wrapper"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                 WORLD_READ WORLD_EXECUTE)

# ---------------------------------------------------------------------------
# etc/mono in the build tree
# ---------------------------------------------------------------------------
# Same layout as the installed $sysconfdir/mono, but the dllmap entries point
# at the helper libraries in the build tree so an uninstalled runtime does not
# pick up a system-wide mono's copies.
# 1.0 is build-tree only and comes from net_1_1; there is no installed 1.0
# directory.
configure_file("${CMAKE_SOURCE_DIR}/data/net_1_1/machine.config"
               "${MONO_BUILD_CFG_DIR}/mono/1.0/machine.config" COPYONLY)
foreach(_v 2.0 4.0 4.5)
  string(REPLACE "." "_" _dir "net_${_v}")
  foreach(_f machine.config web.config)
    configure_file("${CMAKE_SOURCE_DIR}/data/${_dir}/${_f}"
                   "${MONO_BUILD_CFG_DIR}/mono/${_v}/${_f}" COPYONLY)
  endforeach()
  configure_file("${CMAKE_SOURCE_DIR}/data/Browsers/Compat.browser"
                 "${MONO_BUILD_CFG_DIR}/mono/${_v}/Browsers/Compat.browser" COPYONLY)
endforeach()
configure_file("${CMAKE_SOURCE_DIR}/data/browscap.ini"
               "${MONO_BUILD_CFG_DIR}/mono/browscap.ini" COPYONLY)

add_custom_command(
  OUTPUT "${MONO_BUILD_CFG_DIR}/mono/config"
  COMMAND ${CMAKE_COMMAND}
          -DIN=${CMAKE_BINARY_DIR}/data/config
          -DOUT=${MONO_BUILD_CFG_DIR}/mono/config
          -DBUILD_DIR=${CMAKE_BINARY_DIR}
          -P "${CMAKE_SOURCE_DIR}/cmake/MonoBuildTreeConfig.cmake"
  DEPENDS "${CMAKE_BINARY_DIR}/data/config"
          "${CMAKE_SOURCE_DIR}/cmake/MonoBuildTreeConfig.cmake"
  COMMENT "Generating build-tree etc/mono/config"
  VERBATIM)
add_custom_target(mono-build-config ALL
  DEPENDS "${MONO_BUILD_CFG_DIR}/mono/config")

# ---------------------------------------------------------------------------
# _tmpinst/bin -- the tiny PATH shims the class-library build shells out to.
# ---------------------------------------------------------------------------
set(_tmpinst "${CMAKE_CURRENT_BINARY_DIR}/_tmpinst/bin")
file(MAKE_DIRECTORY "${_tmpinst}")

configure_file("${CMAKE_CURRENT_BINARY_DIR}/mono-wrapper" "${_tmpinst}/mono" COPYONLY)

function(_mono_tmpinst_shim name target)
  cmake_parse_arguments(ARG "" "RUNTIME" "" ${ARGN})
  set(_on "${_tmpinst}/mono")
  if(ARG_RUNTIME)
    set(_on "${ARG_RUNTIME}")
  endif()
  file(WRITE "${_tmpinst}/${name}"
       "#! /bin/sh\nexec \"${_on}\" \"${target}\" \"$@\"\n")
  file(CHMOD "${_tmpinst}/${name}" PERMISSIONS
       OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
       WORLD_READ WORLD_EXECUTE)
endfunction()

# csc follows MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS like every other C# compile.
# mcs, ilasm and al do not: they are assemblies out of this tree, built against
# this tree's corlib.
_mono_tmpinst_shim(csc   "${MONO_CSC}" RUNTIME "${MONO_TOOLS_RUNTIME_HOST}")
_mono_tmpinst_shim(mcs   "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}/mcs.exe")
_mono_tmpinst_shim(ilasm "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}/ilasm.exe")
_mono_tmpinst_shim(al    "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}/al.exe")

# ---------------------------------------------------------------------------
# The class libraries
# ---------------------------------------------------------------------------
# mcs/ used to be driven by its own recursive make system from here.  It is
# CMake now and lives in mcs/CMakeLists.txt; all that is left is the sanity
# check, because the shims above are what that build shells out to.
if(MONO_ENABLE_MCS_BUILD AND NOT MONO_ENABLE_EXECUTABLES)
  message(FATAL_ERROR
    "MONO_ENABLE_MCS_BUILD needs MONO_ENABLE_EXECUTABLES: the class libraries "
    "are compiled by Roslyn running on the runtime this build produces")
endif()
