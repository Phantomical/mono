mono_declare_managed(
  PROGRAM
  NAME       ccrewrite.exe
  PROFILES   net_4_x
  REFS       Mono.CodeContracts System System.Core
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
