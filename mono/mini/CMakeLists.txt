# The runtime half of the JIT: everything the LLVM backend under mono/llvm/
# links against.

# Generated sources

# FULL_VERSION is "<branch>/<sha>" for a git checkout, captured the first time
# the build writes version.h.  The custom command has no inputs, so a later
# commit in the same build directory does not rerun it.  Delete version.h to
# record a newer one.
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

# libmini is shared between mono-sgen and mono-boehm.  Nothing in it has a
# compile-time dependency on either collector.
set(mini_sources
  mini.c
  mini-runtime.c
  seq-points.c
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
  # what both engines dump through
  jit-dump.cpp
  # amd64
  arch-amd64.c
  interp-amd64.c
  exceptions-amd64.c
  tramp-amd64.c
  mini-amd64-gsharedvt.c
  tramp-amd64-gsharedvt.c
  # generated
  "${CMAKE_CURRENT_BINARY_DIR}/version.h")

# The host file: signals, stack overflow, the profiler's sampling timer and the
# runtime's own thread setup, all of which the two hosts do differently enough
# that there is no shared body.  On Windows it brings three shims with it -- the
# DLL entry point, the TLS callback that gives a thread its runtime state, and
# the DAC surface a debugger reads a crashed process through.
if(MONO_HOST_WINDOWS)
  # mini-windows-dlldac.c is not here: it carries a DllMain and a memory
  # reader of its own, for the separate DAC image a debugger loads to walk a
  # crashed process, and both names collide with the runtime's.
  list(APPEND mini_sources
    mini-windows.c
    mini-windows-dllmain.c
    mini-windows-tls-callback.c)
else()
  list(APPEND mini_sources mini-posix.c)
endif()

# The JIT icalls, one file per icall or per family that shares a helper.  Globbed
# so that adding an icall is adding a file; jit-icalls.h declares the lot.
file(GLOB icall_sources CONFIGURE_DEPENDS icalls/*.c)
list(APPEND mini_sources ${icall_sources})

if(MONO_ENABLE_LLVM)
  list(APPEND mini_sources llvm-runtime.cpp domain-method.cpp method-override.cpp thunk.cpp)
endif()

# Only a player build defines these: the operator new family and the
# basic_string members that would otherwise allocate on the engine heap.
if(MONO_UNITY_BUILD)
  list(APPEND mini_sources mono-shim.cpp)
endif()

mono_add_object_library(mini
  SOURCES ${mini_sources}
  INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    # thunk.cpp reaches into mono/llvm/debugging/perf/, whose headers
    # include each other relative to this directory - the mirror of
    # mono/llvm/CMakeLists.txt including ../mini for the same reason.
    "${CMAKE_CURRENT_SOURCE_DIR}/../llvm"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src"
  DEPENDS mono::hidden mono::eglib_headers mono::llvm)
add_dependencies(mini mono-version)

# The debugger agent: its own object library because it can be built as a
# loadable module in configurations that disable it from the runtime proper.
# The interpreter is the same shape and lives in mono/interp.
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

# The object sets that make up a complete runtime, per collector.
set(_mini_extra "")
if(MONO_ENABLE_INTERPRETER)
  list(APPEND _mini_extra mono_ee_interp)
endif()
if(MONO_ENABLE_DEBUGGER_AGENT)
  list(APPEND _mini_extra mono_dbg)
endif()

# The backend is an object library of its own, and a build configured without
# MONO_LLVM_PREFIX does not have one.
if(MONO_ENABLE_LLVM)
  list(APPEND _mini_extra mono-llvm)
endif()

# Object libraries do not travel through an intermediate link, so each final
# target names the whole set.
set(MONO_SGEN_OBJECTS
    mini ${_mini_extra}
    monoruntime_sgen monoruntime_config monoruntime_support monoculture
    monosgen monoutils eglib
    CACHE INTERNAL "object libraries making up the SGen runtime")
set(MONO_BOEHM_OBJECTS
    mini ${_mini_extra}
    monoruntime_boehm monoruntime_config monoruntime_support monoculture
    monoutils eglib
    CACHE INTERNAL "object libraries making up the Boehm runtime")

if(ZLIB_FOUND)
  set(_mono_zlib ZLIB::ZLIB)
else()
  set(_mono_zlib "")
endif()

# The shared runtimes
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
  target_link_libraries(${name} PRIVATE ${ARG_EXTRA} mono::llvm ${_mono_zlib} mono::platform)
  set_target_properties(${name} PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
    LINKER_LANGUAGE CXX)
  # An ELF library exports everything the compile did not hide, and the runtime
  # relies on that in two directions: the mono binaries call eglib and the
  # collector out of it, and a profiler module dlopen'ed into the process
  # resolves its mono_* against it.  MONO_API marks the embedding surface and
  # nothing else, so a PE image built from the same sources exports far too
  # little.  This asks the linker for the ELF answer.
  set_target_properties(${name} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
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
  target_link_libraries(${name} PUBLIC ${ARG_EXTRA} mono::llvm ${_mono_zlib} mono::platform)
  mono_static_twin_name(${name} "${ARG_OUTPUT_NAME}")
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

# The executables
# build_date is stamped into the binary.  It is regenerated whenever the
# runtime objects are, so it tracks the last real build rather than the last
# configure.
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
    target_link_libraries(${name} PRIVATE ${ARG_EXTRA} mono::llvm ${_mono_zlib} mono::platform)
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

  # `mono` is whichever collector is the default, under the plain name.  A
  # symlink on Windows needs a privilege an ordinary account does not hold, so
  # there it is a copy and the two names are two files.
  set(_mono_alias "${CMAKE_CURRENT_BINARY_DIR}/mono${CMAKE_EXECUTABLE_SUFFIX}")
  if(WIN32)
    set(_mono_alias_command ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:mono-${MONO_DEFAULT_GC_SUFFIX}>" "${_mono_alias}")
  else()
    set(_mono_alias_command ${CMAKE_COMMAND} -E create_symlink
        mono-${MONO_DEFAULT_GC_SUFFIX} "${_mono_alias}")
  endif()
  add_custom_command(
    OUTPUT "${_mono_alias}"
    COMMAND ${_mono_alias_command}
    DEPENDS mono-${MONO_DEFAULT_GC_SUFFIX}
    VERBATIM)
  add_custom_target(mono-symlink ALL DEPENDS "${_mono_alias}")

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

# The runtime loads the override assembly out of its own directory.  So the
# build puts it beside the runtime libraries rather than under mcs/ with the
# class libraries.
if(MONO_ENABLE_MCS_BUILD)
  set(_overrides "${CMAKE_CURRENT_BINARY_DIR}/mono-overrides.dll")
  mono_tools_runtime(_overrides_host MONO_PATH "${MONO_MCS_LIBDIR}/build")
  add_custom_command(
    OUTPUT "${_overrides}"
    COMMAND ${_overrides_host} "${MONO_CSC}" -debug:portable -noconfig -nologo
            -target:library "-out:${_overrides}"
            # Against this tree's corlib, which is where the two attributes it
            # carries live.  Without -nostdlib the compile binds to whatever
            # mscorlib the compiler host offers, which is the machine's mono
            # when MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS is on.
            -nostdlib "-r:${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}/mscorlib.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/mono-overrides.cs"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mono-overrides.cs" mcs
    COMMENT "CSC mono-overrides.dll"
    VERBATIM)
  add_custom_target(mono-overrides ALL DEPENDS "${_overrides}")
  mono_tools_runtime_depends(mono-overrides)
  install(FILES "${_overrides}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()

if(BUILD_TESTING)
  include(MonoMiniTests)
endif()
