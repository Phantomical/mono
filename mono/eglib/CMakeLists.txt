# The embedded glib replacement.  Everything the runtime uses from glib lives
# here.  There is no --with-glib=system path any more.

# eglib-config.h says what the host's C library and compiler give eglib.  On
# Windows the answer is written out rather than probed: eglib-config.hw is the
# copy the msvc/ project files use, and taking it here keeps one answer for
# both builds.  MSVC has none of the __attribute__ spellings the template
# fills in, so there is nothing for a probe to find.
if(MONO_HOST_WINDOWS)
  configure_file(eglib-config.hw "${CMAKE_CURRENT_BINARY_DIR}/eglib-config.h" COPYONLY)
else()
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
endif()

# The half of eglib that is the same everywhere.
set(eglib_sources
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
  gspawn.c)

# The rest is one file per subject with a suffix naming the host.  gunicode's
# win32 half is an addition rather than a replacement: it holds the code page
# conversions, which the portable file leaves to the host.
if(MONO_HOST_WINDOWS)
  list(APPEND eglib_sources
    gdate-win32.c
    gdir-win32.c
    gfile-win32.c
    gmisc-win32.c
    gmodule-win32.c
    gtimer-win32.c
    gunicode-win32.c)
else()
  list(APPEND eglib_sources
    gdate-unix.c
    gdir-unix.c
    gfile-unix.c
    gmisc-unix.c
    gmodule-unix.c
    gtimer-unix.c
    gmodule-aix.c)
endif()

if(MONO_HOST_WINDOWS)
  # /Zi rather than -g, and neither _GNU_SOURCE nor _FORTIFY_SOURCE means
  # anything to the MSVC CRT.  eglib's visible symbols need no flag here: see
  # the note in the else arm.
  set(eglib_options    /Zi)
  set(eglib_definitions "")
else()
  # eglib is the one place that keeps its symbols visible: it is linked into
  # both libmono and the standalone helper libraries.  -fvisibility=hidden here
  # would hide the g_* the helpers call.  _FORTIFY_SOURCE is likewise
  # eglib-only, matching the automake build.
  set(eglib_options    -g -Wall)
  set(eglib_definitions _GNU_SOURCE _FORTIFY_SOURCE=2)
endif()

mono_add_object_library(eglib
  SOURCES ${eglib_sources}
  INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
  OPTIONS ${eglib_options}
  DEFINITIONS ${eglib_definitions})

# What consumers of eglib need on their include path.  Attached to the object
# library so `target_link_libraries(foo PRIVATE eglib)` is enough.
target_include_directories(eglib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(eglib PUBLIC mono::platform)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/eglib-config.h"
        DESTINATION "${MONO_DATA_SUBDIR}/mono/eglib")

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
