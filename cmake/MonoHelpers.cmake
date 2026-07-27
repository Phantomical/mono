# Small helpers shared by the per-directory build files.

# The runtime is assembled out of OBJECT libraries rather than static archives.
# automake's noinst_LTLIBRARIES were "convenience libraries": every object in
# them ends up in the shared library whether or not anything references it,
# which is what libmono needs -- most of its exported surface is reached only
# through icalls and P/Invoke, so a static archive would drop it at link time.
# OBJECT libraries give exactly that, without --whole-archive.
#
# Object libraries do not propagate their objects through an intermediate
# link, so every final target lists the full set it needs.
function(mono_add_object_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEFINITIONS;OPTIONS;INCLUDES;DEPENDS" ${ARGN})
  add_library(${name} OBJECT ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE mono::common ${ARG_DEPENDS})
  if(ARG_DEFINITIONS)
    target_compile_definitions(${name} PRIVATE ${ARG_DEFINITIONS})
  endif()
  if(ARG_OPTIONS)
    target_compile_options(${name} PRIVATE ${ARG_OPTIONS})
  endif()
  if(ARG_INCLUDES)
    # The generated config.h goes first.  Some of the vendored trees these
    # targets include from -- bdwgc, libatomic_ops -- ship a config.h of their
    # own, and PRIVATE directories are searched ahead of the ${CMAKE_BINARY_DIR}
    # that mono::common contributes.  In a tree where autotools has ever run
    # there are real files at those paths, so `#include <config.h>` lands on one
    # with none of mono's defines and the arch dispatch collapses into
    # "Don't know how to do memory barriers!".
    target_include_directories(${name} PRIVATE "${CMAKE_BINARY_DIR}" ${ARG_INCLUDES})
  endif()
endfunction()

# `configure_file`-style generation of the little shell wrappers under
# scripts/ and runtime/, which are @VAR@ templates in the autotools build too.
function(mono_configure_script input output)
  configure_file("${input}" "${output}" @ONLY)
  file(CHMOD "${output}" PERMISSIONS
       OWNER_READ OWNER_WRITE OWNER_EXECUTE
       GROUP_READ GROUP_EXECUTE
       WORLD_READ WORLD_EXECUTE)
endfunction()

# Install directories the autotools build spelled out by hand.
set(MONO_ASSEMBLIES_DIR "${CMAKE_INSTALL_FULL_LIBDIR}")
set(MONO_CFG_DIR        "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
set(MONO_INCLUDE_SUBDIR "${CMAKE_INSTALL_INCLUDEDIR}/mono-${MONO_API_VERSION}")
set(MONO_DATA_SUBDIR    "${CMAKE_INSTALL_DATADIR}/mono-${MONO_API_VERSION}")

# ${prefix}-relative libdir, for the .pc files and the relocatable lookup the
# runtime does at startup.
file(RELATIVE_PATH MONO_RELOC_LIBDIR "${CMAKE_INSTALL_PREFIX}" "${CMAKE_INSTALL_FULL_LIBDIR}")

# Where the class libraries live, in the source tree and once installed.
set(MONO_MCS_TOPDIR "${CMAKE_SOURCE_DIR}/mcs")
set(MONO_DEFAULT_PROFILE "net_4_x")

# The Roslyn compilers that drive the class-library build, from the
# roslyn-binaries submodule.  Named here because both runtime/ and the mini
# test wiring need them.
set(MONO_CSC  "${CMAKE_SOURCE_DIR}/external/roslyn-binaries/Microsoft.Net.Compilers/3.7.0/csc.exe")
set(MONO_VBCS "${CMAKE_SOURCE_DIR}/external/roslyn-binaries/Microsoft.Net.Compilers/3.7.0/VBCSCompiler.exe")
