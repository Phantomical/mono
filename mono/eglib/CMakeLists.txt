# The embedded glib replacement.  Everything the runtime uses from glib lives
# here.  There is no --with-glib=system path any more.

set(GNUC_PRETTY    "")
set(GNUC_UNUSED    "__attribute__((__unused__))")
set(GNUC_NORETURN  "__attribute__((__noreturn__))")
set(SEARCHSEP      ":")
set(PATHSEP        "/")
set(BREAKPOINT     "G_STMT_START { __asm__ (\"int $03\"); } G_STMT_END")
set(OS             "UNIX")
set(GSIZE          "size_t")
set(GSSIZE         "ptrdiff_t")
set(GSIZE_FORMAT   "\"lu\"")
set(PIDTYPE        "pid_t")
if(WORDS_BIGENDIAN)
  set(ORDER "G_BIG_ENDIAN")
else()
  set(ORDER "G_LITTLE_ENDIAN")
endif()
foreach(_v HAVE_ALLOCA_H HAVE_UNISTD_H)
  if(${_v})
    set(${_v} 1)
  else()
    set(${_v} 0)
  endif()
endforeach()

configure_file(eglib-config.h.in "${CMAKE_CURRENT_BINARY_DIR}/eglib-config.h" @ONLY)

mono_add_object_library(eglib
  SOURCES
    garray.c
    gbytearray.c
    gerror.c
    ghashtable.c
    giconv.c
    gmem.c
    goutput.c
    gqsort.c
    gstr.c
    gslist.c
    gstring.c
    gptrarray.c
    glist.c
    gqueue.c
    gpath.c
    gshell.c
    gfile.c
    gfile-posix.c
    gpattern.c
    gmarkup.c
    gutf8.c
    gunicode.c
    gdate-unix.c
    gdir-unix.c
    gfile-unix.c
    gmisc-unix.c
    gmodule-unix.c
    gtimer-unix.c
    gmodule-aix.c
    gspawn.c
  INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
  # eglib is the one place that keeps its symbols visible: it is linked into
  # both libmono and the standalone helper libraries.  -fvisibility=hidden here
  # would hide the g_* the helpers call.  _FORTIFY_SOURCE is likewise
  # eglib-only, matching the automake build.
  OPTIONS -g -Wall
  DEFINITIONS _GNU_SOURCE _FORTIFY_SOURCE=2)

# What consumers of eglib need on their include path.  Attached to the object
# library so `target_link_libraries(foo PRIVATE eglib)` is enough.
target_include_directories(eglib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(eglib PUBLIC m)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/eglib-config.h"
        DESTINATION "${MONO_DATA_SUBDIR}/mono/eglib")

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
