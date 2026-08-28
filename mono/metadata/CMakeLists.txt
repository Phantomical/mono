# The runtime's object model, loader, marshaller and icalls.
#
# Everything here is compiled twice when both collectors are enabled, because
# a handful of translation units (gc.c, monitor.c, object.c, ...) inline the
# collector's allocation and write-barrier paths.  The two object libraries
# differ in their GC defines (-DHAVE_BOEHM_GC plus -DGC_THREADS, against
# -DHAVE_SGEN_GC) and in which GC glue file they pull in.

# The Win32 objects the runtime's object model is written against -- handles,
# events, mutexes, sockets, the console -- are the host's own on Windows and
# emulated everywhere else.  So each of these is a pair, and the file that
# names the other host does not compile here.  A file named after a platform
# and not in this list guards its own body instead.
if(MONO_HOST_WINDOWS)
  set(metadata_platform_sources
    console-win32.c
    w32file-win32.c
    icall-windows.c
    marshal-windows.c
    mono-security-windows.c
    w32mutex-win32.c
    w32semaphore-win32.c
    w32event-win32.c
    w32process-win32.c
    w32socket-win32.c
    w32error-win32.c)
else()
  set(metadata_platform_sources
    console-unix.c
    w32mutex-unix.c
    w32semaphore-unix.c
    w32event-unix.c
    w32process-unix.c
    w32process-unix-osx.c
    w32process-unix-bsd.c
    w32process-unix-haiku.c
    w32process-unix-default.c
    w32socket-unix.c
    w32file-unix.c
    w32file-unix-glob.c
    w32error-unix.c)
endif()

set(metadata_common_sources
  ${metadata_platform_sources}
  threadpool-worker-default.c
  # unity additions
  unity-memory-info.c
  unity-utils.c
  unity-icall.c
  # core
  oop.c
  appdomain.c
  assembly-load-context.c
  domain.c
  assembly.c
  attach.c
  class.c
  class-init.c
  class-setup-vtable.c
  class-accessors.c
  cominterop.c
  coree.c
  debug-helpers.c
  debug-mono-symfile.c
  debug-mono-ppdb.c
  environment.c
  icall-eventpipe.c
  exception.c
  w32file.c
  filewatcher.c
  icall.c
  image.c
  jit-info.c
  loader.c
  loaded-images.c
  loaded-images-global.c
  loaded-images-netcore.c
  lock-tracer.c
  marshal.c
  marshal-noilgen.c
  mempool.c
  metadata.c
  metadata-verify.c
  metadata-update.c
  method-builder.c
  mono-basic-block.c
  mono-config.c
  mono-debug.c
  mono-endian.c
  mono-hash.c
  mono-conc-hash.c
  mono-mlist.c
  mono-perfcounters.c
  mono-route.c
  object.c
  opcodes.c
  property-bag.c
  w32socket.c
  w32process.c
  profiler.c
  rand.c
  remoting.c
  runtime.c
  mono-security.c
  security-core-clr.c
  security-manager.c
  string-icalls.c
  sysmath.c
  threads.c
  threadpool.c
  threadpool-io.c
  verify.c
  dynamic-stream.c
  file-mmap-posix.c
  file-mmap-windows.c
  metadata-cross-helpers.c
  seq-points-data.c
  handle.c
  w32handle-namespace.c
  w32handle.c
  reflection.c
  dynamic-image.c
  sre.c
  sre-encode.c
  sre-save.c
  custom-attrs.c
  fdhandle.c
  callspec.c
  abi.c
  memory-manager.c
  native-library.c
  native-library-qcall.c
  etw-profiler.c)

if(MONO_ENABLE_ICALL_TABLES)
  list(APPEND metadata_common_sources icall-table.c)
endif()

if(MONO_ENABLE_ILGEN)
  list(APPEND metadata_common_sources
    method-builder-ilgen.c
    marshal-ilgen.c
    sgen-mono-ilgen.c)
endif()

# Compile-time dependencies on the collector.
set(metadata_gc_sources
  gc-stats.c
  gc.c
  monitor.c)

set(metadata_boehm_sources
  null-gc.c
  null-gc-handles.c
  boehm-gc.c
  # Unity's object-graph walk.  The source has an SGen arm as well, but only
  # the Boehm variant compiles it.
  unity-liveness.c)

set(metadata_sgen_sources
  sgen-bridge.c
  sgen-old-bridge.c
  sgen-new-bridge.c
  sgen-tarjan-bridge.c
  sgen-toggleref.c
  sgen-stw.c
  sgen-mono.c)

# mono-config-dirs.c is a library of its own so that changing --prefix only
# recompiles this one file instead of invalidating ccache for the whole tree.
mono_add_object_library(monoruntime_config
  SOURCES mono-config-dirs.c
  DEFINITIONS
    MONO_BINDIR="${CMAKE_INSTALL_FULL_BINDIR}/"
    MONO_ASSEMBLIES="${MONO_ASSEMBLIES_DIR}"
    MONO_CFG_DIR="${MONO_CFG_DIR}"
    MONO_RELOC_LIBDIR="../${MONO_RELOC_LIBDIR}"
  INCLUDES "${CMAKE_SOURCE_DIR}/mono"
  DEPENDS mono::hidden mono::eglib_headers)

# support.c is an empty translation unit except under ENABLE_MONOTOUCH or
# ENABLE_MONODROID, where it pulls in the zlib helper.  HAVE_UNISTD_H and
# HAVE_HIDDEN are the defines the zlib headers read.
mono_add_object_library(monoruntime_support
  SOURCES support.c
  DEFINITIONS HAVE_HIDDEN
  INCLUDES "${CMAKE_SOURCE_DIR}/mono"
  DEPENDS mono::hidden mono::eglib_headers)

# The two collector flavours
function(_mono_add_metadata_variant name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEFINITIONS" ${ARGN})
  mono_add_object_library(${name}
    SOURCES ${metadata_common_sources} ${metadata_gc_sources} ${ARG_SOURCES}
    DEFINITIONS ${ARG_DEFINITIONS} NO_GLOBALIZATION_SHIM
    INCLUDES "${CMAKE_SOURCE_DIR}/mono"
    DEPENDS mono::hidden mono::eglib_headers)
endfunction()

if(MONO_ENABLE_BOEHM)
  _mono_add_metadata_variant(monoruntime_boehm
    SOURCES ${metadata_boehm_sources}
    DEFINITIONS HAVE_BOEHM_GC GC_THREADS)
  target_include_directories(monoruntime_boehm PRIVATE
    "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src"
    # boehm-dirty-table.h.  Its implementation is built with the collector
    # rather than with us, which is the point of it -- see cmake/bdwgc.
    "${CMAKE_SOURCE_DIR}/cmake/bdwgc")
endif()

if(MONO_ENABLE_SGEN)
  _mono_add_metadata_variant(monoruntime_sgen
    SOURCES ${metadata_sgen_sources}
    DEFINITIONS HAVE_SGEN_GC)
endif()

install(FILES
    appdomain.h
    assembly.h
    attrdefs.h
    blob.h
    class.h
    debug-helpers.h
    debug-mono-symfile.h
    environment.h
    exception.h
    image.h
    loader.h
    metadata.h
    mono-config.h
    mono-debug.h
    mono-gc.h
    mono-private-unstable.h
    object.h
    object-forward.h
    opcodes.h
    profiler.h
    profiler-events.h
    reflection.h
    row-indexes.h
    sgen-bridge.h
    threads.h
    tokentype.h
    verify.h
  DESTINATION "${MONO_INCLUDE_SUBDIR}/mono/metadata")
