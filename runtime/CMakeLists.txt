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
_mono_tmpinst_shim(csc   "${MONO_CSC}" RUNTIME "${MONO_CSC_HOST}")
_mono_tmpinst_shim(mcs   "${MONO_MCS_TOPDIR}/class/lib/${MONO_DEFAULT_PROFILE}/mcs.exe")
_mono_tmpinst_shim(ilasm "${MONO_MCS_TOPDIR}/ilasm/ilasm.exe")
_mono_tmpinst_shim(al    "${MONO_MCS_TOPDIR}/class/lib/${MONO_DEFAULT_PROFILE}/al.exe")

# ---------------------------------------------------------------------------
# mcs/build/config.make -- how the class-library make system learns about this
# build.  It is written into the source tree because that is where mcs looks
# for it; a second build directory over the same checkout will overwrite it.
# ---------------------------------------------------------------------------
if(MONO_ENABLE_MCS_BUILD AND NOT MONO_ENABLE_EXECUTABLES)
  message(FATAL_ERROR
    "MONO_ENABLE_MCS_BUILD needs MONO_ENABLE_EXECUTABLES: the class libraries "
    "are compiled by Roslyn running on the runtime this build produces")
endif()

if(MONO_ENABLE_MCS_BUILD)
  # The generator may be ninja; the class-library build needs GNU make.
  find_program(MONO_GNU_MAKE NAMES gmake make REQUIRED)

  set(_profiles "${MONO_MCS_PROFILES}")
  if(_profiles MATCHES "net_4_x")
    set(_profiles "binary_reference_assemblies ${_profiles} xbuild_12 xbuild_14")
  endif()
  if(MONO_WITH_UNITYJIT)
    string(APPEND _profiles " unityjit")
  endif()
  if(MONO_WITH_UNITYAOT)
    string(APPEND _profiles " unityaot")
  endif()
  string(STRIP "${_profiles}" MONO_BUILD_PROFILES)

  if(MONO_ENABLE_BTLS)
    set(_have_btls "HAVE_BTLS=1")
  else()
    set(_have_btls "")
  endif()
  if(MONO_ENABLE_MONO_NATIVE)
    set(_mono_native_supported "true")
  else()
    set(_mono_native_supported "false")
  endif()

  # Move just the C# compiler off $(RUNTIME) when asked; rules.make defaults
  # CSC_RUNTIME to $(RUNTIME), so saying nothing leaves the build as it was.
  # The tools that surround csc stay put -- they load out of class/lib/build,
  # which only the runtime being built can read.
  if(MONO_TOOLS_RUNTIME_IS_SYSTEM)
    set(_csc_runtime "CSC_RUNTIME = ${MONO_CSC_HOST}")
  else()
    set(_csc_runtime "")
  endif()

  configure_file("${CMAKE_SOURCE_DIR}/cmake/mcs-config.make.in"
                 "${MONO_MCS_TOPDIR}/build/config.make" @ONLY)

  # `all-profiles` needs a working runtime, the build-tree config, and the
  # shims above already on disk.
  add_custom_target(mcs ALL
    COMMAND ${CMAKE_COMMAND} -E env
            "PATH=${_tmpinst}:$ENV{PATH}"
            "${MONO_GNU_MAKE}" --no-print-directory -s
            -C "${MONO_MCS_TOPDIR}"
            NO_DIR_CHECK=1
            "PROFILES=${MONO_BUILD_PROFILES}"
            "CC=${CMAKE_C_COMPILER}"
            all-profiles
    DEPENDS mono-build-config
    COMMENT "Building the class libraries (mcs)"
    USES_TERMINAL
    VERBATIM)
  # The class-library build needs the runtime, and its AOT step invokes
  # $(mono_build_root)/mono/mini/mono by name, so the symlink has to exist too.
  add_dependencies(mcs mono-${MONO_DEFAULT_GC_SUFFIX} mono-symlink)

  install(CODE "
    execute_process(
      COMMAND \"${MONO_GNU_MAKE}\" -C \"${MONO_MCS_TOPDIR}\"
              NO_DIR_CHECK=1 \"PROFILES=${MONO_BUILD_PROFILES}\"
              RUNTIME_HAS_CONSISTENT_GACDIR=yes
              \"prefix=\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}\"
              install-profiles
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR \"mcs install-profiles failed\")
    endif()")
endif()
