mono_declare_managed(
  PROGRAM
  NAME       ikdasm.exe
  PROFILES   net_4_x
  REFS       System System.Core System.Security
  FLAGS      -d:NO_SYMBOL_WRITER
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
