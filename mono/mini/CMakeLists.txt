# The runtime half of the JIT: everything the LLVM backend under mono/llvm/
# links against.

# ---------------------------------------------------------------------------
# Generated sources
# ---------------------------------------------------------------------------

# FULL_VERSION is "<branch>/<sha>" for a git checkout.  Regenerated on every
# build, but written through copy_if_different so an unchanged HEAD does not
# relink the world.
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/version.h"
  COMMAND ${CMAKE_COMMAND}
          -DSRC_DIR=${CMAKE_SOURCE_DIR}
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/version.h
          -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
          -P "${CMAKE_SOURCE_DIR}/cmake/MonoGenVersion.cmake"
  COMMENT "Generating version.h"
  VERBATIM)
add_custom_target(mono-version DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/version.h")

# ---------------------------------------------------------------------------
# libmini -- shared between mono-sgen and mono-boehm; nothing in here has a
# compile-time dependency on either collector.
# ---------------------------------------------------------------------------
set(mini_sources
  mini.c
  mini-runtime.c
  seq-points.c
  jit-icalls.c
  trace.c
  driver.c
  debug-mini.c
  aot-runtime.c
  mini-exceptions.c
  mini-trampolines.c
  mini-generic-sharing.c
  tasklets.c
  mini-native-types.c
  unwind.c
  mini-gc.c
  debugger-agent-stubs.c
  mini-cross-helpers.c
  lldb.c
  mixed_callstack_plugin.c
  mini-profiler.c
  interp-stubs.c
  llvmonly-runtime.c
  monovm.c
  mini-unity.c
  # amd64
  arch-amd64.c
  interp-amd64.c
  exceptions-amd64.c
  tramp-amd64.c
  mini-amd64-gsharedvt.c
  tramp-amd64-gsharedvt.c
  # posix
  mini-posix.c
  # generated
  "${CMAKE_CURRENT_BINARY_DIR}/version.h")

if(MONO_ENABLE_LLVM)
  list(APPEND mini_sources llvm-runtime.cpp domain-method.cpp method-override.cpp thunk.cpp)
endif()

mono_add_object_library(mini
  SOURCES ${mini_sources}
  INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src"
  DEPENDS mono::hidden mono::eglib_headers mono::llvm)
add_dependencies(mini mono-version)

# ---------------------------------------------------------------------------
# The debugger agent: its own object library because it can be built as a
# loadable module in configurations that disable it from the runtime proper.
# The interpreter is the same shape and lives in mono/interp.
# ---------------------------------------------------------------------------
if(MONO_ENABLE_DEBUGGER_AGENT)
  mono_add_object_library(mono_dbg
    SOURCES
      debugger-agent.c
      debugger-state-machine.c
      debugger-engine.c
    INCLUDES
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${CMAKE_CURRENT_BINARY_DIR}"
      "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
      "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src"
    DEPENDS mono::hidden mono::eglib_headers mono::llvm)
  add_dependencies(mono_dbg mono-version)
endif()

# ---------------------------------------------------------------------------
# The object sets that make up a complete runtime, per collector.
# ---------------------------------------------------------------------------
set(_mini_extra "")
if(MONO_ENABLE_INTERPRETER)
  list(APPEND _mini_extra mono_ee_interp)
endif()
if(MONO_ENABLE_DEBUGGER_AGENT)
  list(APPEND _mini_extra mono_dbg)
endif()

# Object libraries do not travel through an intermediate link, so each final
# target names the whole set.
# Cached so mono/unit-tests can link the same set without re-deriving it.
set(MONO_SGEN_OBJECTS
    mini ${_mini_extra}
    monoruntime_sgen monoruntime_config monoruntime_support monoculture
    monosgen monoutils eglib
    mono-llvm
    CACHE INTERNAL "object libraries making up the SGen runtime")
set(MONO_BOEHM_OBJECTS
    mini ${_mini_extra}
    monoruntime_boehm monoruntime_config monoruntime_support monoculture
    monoutils eglib
    mono-llvm
    CACHE INTERNAL "object libraries making up the Boehm runtime")

if(ZLIB_FOUND)
  set(_mono_zlib ZLIB::ZLIB)
else()
  set(_mono_zlib "")
endif()

# ---------------------------------------------------------------------------
# The shared runtimes
# ---------------------------------------------------------------------------
# libmono has no sources of its own -- it is exactly the union of the object
# libraries, which is how the automake build had it too (an empty _SOURCES and
# a long _LIBADD).  CMake still wants a source file to hang the target off.
set(_mono_placeholder "${CMAKE_CURRENT_BINARY_DIR}/libmono-placeholder.c")
file(GENERATE OUTPUT "${_mono_placeholder}"
     CONTENT "/* libmono is assembled from object libraries; see mono/mini/CMakeLists.txt. */\n")

# The rpath is $ORIGIN because an embedder drops the runtime and the helper
# libraries it dlopens (libmono-native in particular) into one private
# directory.  Without it a bare dlopen name reaches ld.so.cache and picks up
# whatever system mono happens to be installed.
function(_mono_add_runtime_library name)
  cmake_parse_arguments(ARG "" "" "OBJECTS;EXTRA" ${ARGN})
  add_library(${name} SHARED "${_mono_placeholder}")
  foreach(_o IN LISTS ARG_OBJECTS)
    target_link_libraries(${name} PRIVATE ${_o})
  endforeach()
  target_link_libraries(${name} PRIVATE ${ARG_EXTRA} mono::llvm ${_mono_zlib} m)
  set_target_properties(${name} PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
    LINKER_LANGUAGE CXX)
  install(TARGETS ${name} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endfunction()

if(MONO_ENABLE_SGEN AND MONO_ENABLE_LIBRARIES)
  _mono_add_runtime_library(monosgen-2.0 OBJECTS ${MONO_SGEN_OBJECTS})

endif()
if(MONO_ENABLE_BOEHM AND MONO_ENABLE_LIBRARIES)
  _mono_add_runtime_library(monoboehm-2.0 OBJECTS ${MONO_BOEHM_OBJECTS} EXTRA monogc)
endif()

# libtool built a static archive alongside every shared library, and embedders
# that link the runtime statically consume it.  It is also what the unit tests
# link: they need archive semantics rather than the object-library semantics
# libmono uses, because test-path.c includes mono/utils/mono-path.c to reach its
# statics and force-linking every object would define those symbols twice.
function(_mono_add_runtime_archive name)
  cmake_parse_arguments(ARG "" "OUTPUT_NAME" "OBJECTS;EXTRA" ${ARGN})
  add_library(${name} STATIC "${_mono_placeholder}")
  foreach(_o IN LISTS ARG_OBJECTS)
    target_link_libraries(${name} PUBLIC ${_o})
  endforeach()
  target_link_libraries(${name} PUBLIC ${ARG_EXTRA} mono::llvm ${_mono_zlib} m)
  set_target_properties(${name} PROPERTIES OUTPUT_NAME "${ARG_OUTPUT_NAME}")
  install(TARGETS ${name} ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endfunction()

if(MONO_ENABLE_SGEN AND MONO_ENABLE_LIBRARIES)
  _mono_add_runtime_archive(monosgen-static OUTPUT_NAME monosgen-2.0
                            OBJECTS ${MONO_SGEN_OBJECTS})
endif()
if(MONO_ENABLE_BOEHM AND MONO_ENABLE_LIBRARIES)
  _mono_add_runtime_archive(monoboehm-static OUTPUT_NAME monoboehm-2.0
                            OBJECTS ${MONO_BOEHM_OBJECTS} EXTRA monogc)
endif()

# ---------------------------------------------------------------------------
# The executables
# ---------------------------------------------------------------------------
# build_date is stamped into the binary; it is regenerated whenever the runtime
# objects are, so it tracks the last real build rather than the last configure.
function(_mono_buildver suffix depends)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/buildver-${suffix}.h"
    COMMAND ${CMAKE_COMMAND}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/buildver-${suffix}.h
            -P "${CMAKE_SOURCE_DIR}/cmake/MonoGenBuildVer.cmake"
    DEPENDS ${depends}
    COMMENT "Generating buildver-${suffix}.h"
    VERBATIM)
endfunction()

function(_mono_add_runtime_executable name main suffix)
  cmake_parse_arguments(ARG "" "" "OBJECTS;EXTRA" ${ARGN})
  _mono_buildver(${suffix} "${ARG_OBJECTS}")
  add_executable(${name} ${main} "${CMAKE_CURRENT_BINARY_DIR}/buildver-${suffix}.h")
  target_include_directories(${name} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(${name} PRIVATE mono::common mono::hidden mono::eglib_headers)
  if(MONO_STATIC_MONO)
    # Linking libmini into the binary rather than going through libmono is
    # measurably faster, mostly because TLS accesses stop going through the
    # general dynamic model.
    foreach(_o IN LISTS ARG_OBJECTS)
      target_link_libraries(${name} PRIVATE ${_o})
    endforeach()
    target_link_libraries(${name} PRIVATE ${ARG_EXTRA} mono::llvm ${_mono_zlib} m)
  else()
    target_link_libraries(${name} PRIVATE mono${suffix}-2.0)
  endif()
  # libmini contains C++ (the LLVM backend), so the binary needs the C++
  # runtime even though main.c is C.
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
  install(TARGETS ${name} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endfunction()

if(MONO_ENABLE_EXECUTABLES)
  if(MONO_ENABLE_SGEN)
    _mono_add_runtime_executable(mono-sgen main-sgen.c sgen OBJECTS ${MONO_SGEN_OBJECTS})
  endif()
  if(MONO_ENABLE_BOEHM)
    _mono_add_runtime_executable(mono-boehm main.c boehm OBJECTS ${MONO_BOEHM_OBJECTS} EXTRA monogc)
  endif()

  # `mono` is a symlink to whichever collector is the default.
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mono"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            mono-${MONO_DEFAULT_GC_SUFFIX} "${CMAKE_CURRENT_BINARY_DIR}/mono"
    DEPENDS mono-${MONO_DEFAULT_GC_SUFFIX}
    VERBATIM)
  add_custom_target(mono-symlink ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/mono")

  install(CODE "
    execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
      mono-${MONO_DEFAULT_GC_SUFFIX} \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/mono\")")
endif()

# The un-suffixed libmono-2.0 aliases: embedders link -lmono-2.0 and get
# whichever collector this build made the default.
if(MONO_ENABLE_LIBRARIES)
  install(CODE "
    set(_libdir \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_LIBDIR}\")
    file(GLOB _libs \"\${_libdir}/libmono${MONO_DEFAULT_GC_SUFFIX}-2.0.so*\"
                       \"\${_libdir}/libmono${MONO_DEFAULT_GC_SUFFIX}-2.0.a\")
    foreach(_l IN LISTS _libs)
      get_filename_component(_n \"\${_l}\" NAME)
      string(REPLACE \"${MONO_DEFAULT_GC_SUFFIX}\" \"\" _alias \"\${_n}\")
      execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
                      \"\${_n}\" \"\${_libdir}/\${_alias}\")
    endforeach()")
endif()

install(FILES jit.h mono-private-unstable.h
        DESTINATION "${MONO_INCLUDE_SUBDIR}/mono/jit")

if(BUILD_TESTING)
  include(MonoMiniTests)
endif()
