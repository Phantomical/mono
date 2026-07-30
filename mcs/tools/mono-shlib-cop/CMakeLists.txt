mono_declare_managed(
  PROGRAM
  NAME       mono-shlib-cop.exe
  PROFILES   net_4_x
  REFS       Mono.Posix System System.Xml
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
