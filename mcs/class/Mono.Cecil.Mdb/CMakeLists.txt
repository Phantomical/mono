mono_declare_managed(
  NAME       Mono.Cecil.Mdb.dll
  PROFILES   net_4_x
  KEYFILE    ../mono.snk
  SNK        <topdir>/class/mono.snk
  PACKAGE    none
  REFS       System Mono.Cecil
  FLAGS      -d:CECIL
  NO_INSTALL
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
