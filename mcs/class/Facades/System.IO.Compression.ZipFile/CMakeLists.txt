mono_declare_managed(
  NAME       System.IO.Compression.ZipFile.dll
  PROFILES   net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../ecma.pub
  SNK        <topdir>/class/mono.snk
  INSTALL_DIR <libdir>/mono/4.5/Facades
  REFS       System System.IO.Compression.FileSystem
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
