mono_declare_managed(
  PROGRAM
  NAME       illinkanalyzer.exe
  PROFILES   net_4_x
  REFS       System System.Xml System.Core Mono.Cecil
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
