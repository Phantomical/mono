# The bundled zlib.  Only built when the system does not provide one -- the
# runtime prefers the system copy so that security fixes arrive with the
# distribution rather than with a mono release.

if(ZLIB_FOUND)
  return()
endif()

add_library(monoz OBJECT
  adler32.c
  compress.c
  crc32.c
  uncompr.c
  deflate.c
  trees.c
  zutil.c
  inflate.c
  infback.c
  inftrees.c
  inffast.c)

target_include_directories(monoz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(monoz PRIVATE HAVE_UNISTD_H HAVE_HIDDEN)
# zlib is third-party.  Mono's warning set is not its problem.
target_compile_options(monoz PRIVATE -w)

# Stand in for ZLIB::ZLIB so consumers need no conditional of their own.
add_library(mono_zlib INTERFACE)
add_library(ZLIB::ZLIB ALIAS mono_zlib)
target_link_libraries(mono_zlib INTERFACE monoz)
set(ZLIB_FOUND TRUE PARENT_SCOPE)
