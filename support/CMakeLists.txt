# libMonoPosixHelper -- the native half of Mono.Posix -- and libMonoSupportW,
# the small Windows-API shim the class libraries P/Invoke into.

set(_mph_sources
  # What holds up on any host with a C compiler: the ANSI half of Mono.Posix,
  # and the zip support System.IO.Compression reaches through it.
  errno.c
  map.c
  signal.c
  stdio.c
  string.c
  stdlib.c
  zlib-helper.c
  minizip/ioapi.c
  minizip/unzip.c
  minizip/zip.c)

# The POSIX half.  Each of these wraps a call the Windows CRT does not have, so
# Mono.Posix on Windows gets the ANSI half above and throws for the rest --
# which is what the class libraries there expect.
if(NOT MONO_HOST_WINDOWS)
  list(APPEND _mph_sources
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
    x-struct-str.c)
endif()

add_library(MonoPosixHelper SHARED ${_mph_sources})
# minizip spells its zlib include "../mono/zlib/zlib.h". For that to land on
# mono/zlib/, the search path must be a direct child of the source root, and
# this directory is the obvious one. Relying on the build tree happening to
# sit at <srcroot>/build works only for in-tree build dirs.
target_include_directories(MonoPosixHelper PRIVATE
  "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(MonoPosixHelper PRIVATE
  mono::common mono::eglib_headers eglib)
if(ZLIB_FOUND)
  target_link_libraries(MonoPosixHelper PRIVATE ZLIB::ZLIB)
  target_compile_definitions(MonoPosixHelper PRIVATE HAVE_HIDDEN)
  if(NOT MONO_HOST_WINDOWS)
    target_compile_definitions(MonoPosixHelper PRIVATE HAVE_UNISTD_H)
  endif()
endif()
# glibc has the xattr calls in libc. Elsewhere they come from libattr.
if(NOT HAVE_LSETXATTR)
  if(MONO_XATTR_LIB)
    target_link_libraries(MonoPosixHelper PRIVATE "${MONO_XATTR_LIB}")
  endif()
endif()
# Neither of these libraries carries a version suffix: the class libraries
# dlopen them by bare name. Each library still gets a SONAME.
#
# MonoSupportW stands in for a handful of Win32 entry points -- FindWindowExW
# and its neighbours -- that a few class libraries P/Invoke unconditionally.
# Windows exports them itself, so the shim is a Unix-only library.
if(NOT MONO_HOST_WINDOWS)
  add_library(MonoSupportW SHARED supportw.c support-heap.c)
  target_include_directories(MonoSupportW PRIVATE "${CMAKE_SOURCE_DIR}")
  target_link_libraries(MonoSupportW PRIVATE
    mono::common mono::eglib_headers eglib)
endif()

# Static archives alongside the shared libraries.
add_library(MonoPosixHelper-static STATIC ${_mph_sources})
target_include_directories(MonoPosixHelper-static PRIVATE
  "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(MonoPosixHelper-static PRIVATE mono::common mono::eglib_headers)
if(ZLIB_FOUND)
  target_link_libraries(MonoPosixHelper-static PRIVATE ZLIB::ZLIB)
  target_compile_definitions(MonoPosixHelper-static PRIVATE HAVE_HIDDEN)
  if(NOT MONO_HOST_WINDOWS)
    target_compile_definitions(MonoPosixHelper-static PRIVATE HAVE_UNISTD_H)
  endif()
endif()
mono_static_twin_name(MonoPosixHelper-static MonoPosixHelper)

set(_support_targets MonoPosixHelper MonoPosixHelper-static)
if(NOT MONO_HOST_WINDOWS)
  add_library(MonoSupportW-static STATIC supportw.c support-heap.c)
  target_include_directories(MonoSupportW-static PRIVATE "${CMAKE_SOURCE_DIR}")
  target_link_libraries(MonoSupportW-static PRIVATE mono::common mono::eglib_headers)
  mono_static_twin_name(MonoSupportW-static MonoSupportW)
  list(APPEND _support_targets MonoSupportW MonoSupportW-static)
endif()

install(TARGETS ${_support_targets}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
