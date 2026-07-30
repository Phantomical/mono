mono_declare_managed(
  PROGRAM
  NAME       pdb2mdb.exe
  PROFILES   net_4_x
  REFS       Mono.Cecil Mono.CompilerServices.SymbolWriter System.Core
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
