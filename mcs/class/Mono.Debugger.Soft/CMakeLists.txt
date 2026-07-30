mono_declare_managed(
  NAME       Mono.Debugger.Soft.dll
  PROFILES   net_4_x
  KEYFILE    ../mono.snk
  SNK        ../mono.snk
  REFS       System Mono.Cecil System.Core
  FLAGS      /unsafe -D:MONO_DATACONVERTER_STATIC_METHODS -D:ENABLE_CECIL /publicsign
  TEST_REFS  Mono.Cecil System System.Core
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
