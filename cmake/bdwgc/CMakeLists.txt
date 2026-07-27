# Build external/bdwgc from here rather than from the submodule's own
# CMakeLists.txt: that one predates the options mono needs and would have to be
# patched in-tree.  The submodule stays pristine and this file supplies the
# same configuration mono's configure handed to bdwgc's configure.

set(BDWGC_DIR "${CMAKE_SOURCE_DIR}/external/bdwgc")

if(NOT EXISTS "${BDWGC_DIR}/extra/gc.c")
  message(FATAL_ERROR
    "external/bdwgc is empty -- run `git submodule update --init external/bdwgc`, "
    "or configure with -DMONO_ENABLE_BOEHM=OFF")
endif()

# Read the version out of the submodule instead of hardcoding it, so a bump
# does not silently leave GC_VERSION_* lying.
file(STRINGS "${BDWGC_DIR}/configure.ac" _bdwgc_ac REGEX "^AC_INIT")
string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${_bdwgc_ac}")
set(BDWGC_VERSION_MAJOR ${CMAKE_MATCH_1})
set(BDWGC_VERSION_MINOR ${CMAKE_MATCH_2})
set(BDWGC_VERSION_MICRO ${CMAKE_MATCH_3})
set(BDWGC_VERSION "${BDWGC_VERSION_MAJOR}.${BDWGC_VERSION_MINOR}.${BDWGC_VERSION_MICRO}")

if(MONO_ENABLE_PARALLEL_MARK)
  set(PARALLEL_MARK 1)
endif()

configure_file(config.h.in "${CMAKE_CURRENT_BINARY_DIR}/config.h")

# --enable-single-obj-compilation: the collector is one amalgamated
# translation unit plus the pthread entry-point shim.
add_library(monogc STATIC
  "${BDWGC_DIR}/extra/gc.c"
  "${BDWGC_DIR}/pthread_start.c")

target_include_directories(monogc
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
  PUBLIC  "${BDWGC_DIR}/include"
          "${BDWGC_DIR}/libatomic_ops/src")

target_compile_definitions(monogc
  PRIVATE HAVE_CONFIG_H GC_PTHREAD_START_STANDALONE
  # These have to agree between the collector and mono/metadata/boehm-gc.c,
  # which includes gc.h with the same expectations.
  PUBLIC  GC_LINUX_THREADS _GNU_SOURCE _REENTRANT USE_MMAP USE_MUNMAP
          USE_COMPILER_TLS)

target_compile_options(monogc PRIVATE
  -fno-strict-aliasing
  # The collector deliberately calls a few deprecated APIs.
  -Wno-deprecated-declarations
  -Wall -Wextra -Wno-long-long)

set_target_properties(monogc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(monogc PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
