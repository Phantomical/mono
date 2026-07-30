mono_declare_managed(
  PROGRAM
  NAME       al.exe
  PROFILES   net_4_x
  REFS       System System.Core Mono.Security System.Security Mono.CompilerServices.SymbolWriter
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
