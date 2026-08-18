# libMonoPosixHelper -- the native half of Mono.Posix -- and libMonoSupportW,
# the small Windows-API shim the class libraries P/Invoke into.

set(_mph_sources
  # Portable C
  errno.c
  map.c
  signal.c
  stdio.c
  string.c
  stdlib.c
  # POSIX
  dirent.c
  fcntl.c
  fstab.c
  grp.c
  macros.c
  nl.c
  old-map.c
  pwd.c
  serial.c
  sys-mman.c
  sys-sendfile.c
  sys-socket.c
  sys-stat.c
  sys-statvfs.c
  sys-time.c
  sys-uio.c
  sys-utsname.c
  sys-wait.c
  sys-xattr.c
  syslog.c
  time.c
  unistd.c
  utime.c
  x-struct-str.c
  # zip support for System.IO.Compression
  zlib-helper.c
  minizip/ioapi.c
  minizip/unzip.c
  minizip/zip.c)

add_library(MonoPosixHelper SHARED ${_mph_sources})
# minizip spells its zlib include "../mono/zlib/zlib.h", so it needs a search
# path that is a direct child of the source root for that to land on
# mono/zlib/. This directory is the obvious one; relying on the build tree
# happening to sit at <srcroot>/build works only for in-tree build dirs.
target_include_directories(MonoPosixHelper PRIVATE
  "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(MonoPosixHelper PRIVATE
  mono::common mono::eglib_headers eglib)
if(ZLIB_FOUND)
  target_link_libraries(MonoPosixHelper PRIVATE ZLIB::ZLIB)
  target_compile_definitions(MonoPosixHelper PRIVATE HAVE_UNISTD_H HAVE_HIDDEN)
endif()
# glibc has the xattr calls in libc; elsewhere they come from libattr.
if(NOT HAVE_LSETXATTR)
  if(MONO_XATTR_LIB)
    target_link_libraries(MonoPosixHelper PRIVATE "${MONO_XATTR_LIB}")
  endif()
endif()
# Neither of these libraries carries a version suffix: the class libraries
# dlopen them by bare name.  They do still get a SONAME, which is what libtool's
# -avoid-version produced.
add_library(MonoSupportW SHARED supportw.c support-heap.c)
target_include_directories(MonoSupportW PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(MonoSupportW PRIVATE
  mono::common mono::eglib_headers eglib)

# Static archives alongside the shared libraries, as libtool produced.
add_library(MonoPosixHelper-static STATIC ${_mph_sources})
target_include_directories(MonoPosixHelper-static PRIVATE
  "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(MonoPosixHelper-static PRIVATE mono::common mono::eglib_headers)
if(ZLIB_FOUND)
  target_compile_definitions(MonoPosixHelper-static PRIVATE HAVE_UNISTD_H HAVE_HIDDEN)
endif()
set_target_properties(MonoPosixHelper-static PROPERTIES OUTPUT_NAME MonoPosixHelper)

add_library(MonoSupportW-static STATIC supportw.c support-heap.c)
target_include_directories(MonoSupportW-static PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(MonoSupportW-static PRIVATE mono::common mono::eglib_headers)
set_target_properties(MonoSupportW-static PROPERTIES OUTPUT_NAME MonoSupportW)

install(TARGETS MonoPosixHelper MonoSupportW
                MonoPosixHelper-static MonoSupportW-static
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
