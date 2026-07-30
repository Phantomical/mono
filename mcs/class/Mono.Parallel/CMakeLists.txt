mono_declare_managed(
  NAME       Mono.Parallel.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System
  FLAGS      -d:INSIDE_MONO_PARALLEL
  TEST_REFS  System.Core
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
