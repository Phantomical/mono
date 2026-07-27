# The SGen collector proper.  Nothing in here knows about MonoObject; the
# bindings to the runtime's object model live in mono/metadata/sgen-mono.c.

mono_add_object_library(monosgen
  SOURCES
    sgen-alloc.c
    sgen-cardtable.c
    sgen-debug.c
    sgen-descriptor.c
    sgen-fin-weak-hash.c
    sgen-gc.c
    sgen-gchandles.c
    sgen-gray.c
    sgen-hash-table.c
    sgen-internal.c
    sgen-layout-stats.c
    sgen-los.c
    sgen-marksweep.c
    sgen-memory-governor.c
    sgen-nursery-allocator.c
    sgen-pinning-stats.c
    sgen-pinning.c
    sgen-pointer-queue.c
    sgen-array-list.c
    sgen-protocol.c
    sgen-qsort.c
    sgen-simple-nursery.c
    sgen-split-nursery.c
    sgen-thread-pool.c
    sgen-workers.c
  DEFINITIONS HAVE_SGEN_GC
  INCLUDES "${CMAKE_SOURCE_DIR}/mono"
  DEPENDS mono::hidden mono::eglib_headers)
