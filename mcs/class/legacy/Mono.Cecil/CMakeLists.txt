mono_declare_managed(
  NAME       Mono.Cecil.dll
  PROFILES   net_4_x unityjit
  SUBDIR     legacy
  SNK        ../../mono.snk
  PACKAGE    none
  REFS       System.Core
  FLAGS      -keyfile:../../mono.snk -d:NET_3_5 /publicsign
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
