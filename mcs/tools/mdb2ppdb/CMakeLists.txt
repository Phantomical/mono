mono_declare_managed(
  PROGRAM
  NAME       mdb2ppdb.exe
  PROFILES   net_4_x
  REFS       Mono.Cecil Mono.Cecil.Mdb
  NO_INSTALL
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
