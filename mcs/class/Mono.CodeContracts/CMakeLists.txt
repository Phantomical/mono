mono_declare_managed(
  NAME       Mono.CodeContracts.dll
  PROFILES   net_4_x
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core Mono.Cecil Mono.Cecil.Mdb
  FLAGS      -nowarn:618
  TEST_REFS  System.Core
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
