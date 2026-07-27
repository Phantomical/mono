# libmonoutils: the GC-agnostic, metadata-agnostic bottom layer -- threading,
# memory, TLS, hardware capability probing, logging.

set(monoutils_sources
  dlmalloc.c
  os-event-unix.c
  mono-md5.c
  mono-sha1.c
  mono-logger.c
  mono-codeman.c
  mono-counters.c
  mono-dl.c
  mono-dl-windows.c
  mono-dl-darwin.c
  mono-dl-posix.c
  mono-dl-wasm.c
  mono-experiments.c
  mono-log-windows.c
  mono-log-common.c
  mono-log-posix.c
  mono-log-android.c
  mono-log-darwin.c
  mono-log-flight-recorder.c
  mono-merp.c
  mono-state.c
  mono-internal-hash.c
  mono-io-portability.c
  monobitset.c
  mono-filemap.c
  mono-mmap.c
  mono-mmap-windows.c
  mono-mmap-wasm.c
  mono-os-mutex.c
  mono-flight-recorder.c
  mono-networkinterfaces.c
  mono-proclib.c
  mono-proclib-windows.c
  mono-publib.c
  mono-jemalloc.c
  mono-time.c
  strenc.c
  mono-uri.c
  mono-poll.c
  mono-path.c
  mono-stdlib.c
  mono-property-hash.c
  mono-value-hash.c
  mono-error.c
  mono-context.c
  hazard-pointer.c
  lifo-semaphore.c
  lock-free-queue.c
  lock-free-alloc.c
  lock-free-array-queue.c
  mono-linked-list-set.c
  mono-threads.c
  mono-threads-state-machine.c
  mono-threads-posix.c
  mono-threads-posix-signals.c
  mono-threads-mach.c
  mono-threads-mach-helper.c
  mono-threads-windows.c
  mono-threads-linux.c
  mono-threads-freebsd.c
  mono-threads-netbsd.c
  mono-threads-openbsd.c
  mono-threads-android.c
  mono-threads-haiku.c
  mono-threads-aix.c
  mono-threads-wasm.c
  mono-threads-sunos.c
  mono-threads-coop.c
  mono-utility-thread.c
  mono-tls.c
  mono-utils-debug.c
  atomic.c
  mono-hwcap.c
  bsearch.c
  mono-signal-handler.c
  mono-conc-hashtable.c
  json.c
  networking.c
  networking-posix.c
  networking-fallback.c
  networking-missing.c
  networking-windows.c
  mono-rand.c
  mono-rand-windows.c
  memfuncs.c
  parse.c
  checked-build.c
  options.c
  # Under --enable-cxx the rest of this directory is compiled as C++ and only
  # this file stays C, which is why automake gave it a library of its own.
  # There is no C++ build here, so it is just another source.
  mono-math-c.c
  # amd64-specific
  mach-support-amd64.c
  mono-hwcap-x86.c)

mono_add_object_library(monoutils
  SOURCES ${monoutils_sources}
  INCLUDES "${CMAKE_SOURCE_DIR}/mono"
  DEPENDS mono::hidden mono::eglib_headers)

# mono/utils/mono-compiler.h and friends key off LLVM_API_VERSION even in TUs
# that never touch LLVM.  The autotools build passed 0 here because it asked
# llvm-config for --mono-api-version, which unmodified LLVM does not implement.
target_compile_definitions(monoutils PRIVATE LLVM_API_VERSION=0)

target_include_directories(monoutils PUBLIC "${CMAKE_SOURCE_DIR}/mono")
target_link_libraries(monoutils PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

install(FILES
    mono-logger.h
    mono-error.h
    mono-forward.h
    mono-publib.h
    mono-jemalloc.h
    mono-dl-fallback.h
    mono-private-unstable.h
    mono-counters.h
  DESTINATION "${MONO_INCLUDE_SUBDIR}/mono/utils")
