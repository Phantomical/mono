mono_declare_managed(
  NAME       ICSharpCode.SharpZipLib.dll
  PROFILES   net_4_x unityjit
  KEYFILE    SharpZipLib.pub
  SNK        SharpZipLib.key
  REFS       System System.Xml
  FLAGS      -warn:1
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
