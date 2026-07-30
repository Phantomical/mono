mono_declare_managed(
  PROGRAM
  NAME       mono-api-info.exe
  PROFILES   build net_4_x
  REFS       Mono.Cecil System.Xml System.Core System
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
