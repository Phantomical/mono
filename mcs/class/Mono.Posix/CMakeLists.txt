mono_declare_managed(
  NAME       Mono.Posix.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System
  FLAGS      /unsafe /nowarn:0618,612
  TEST_REFS  Mono.Posix System
  TEST_FLAGS /unsafe /nowarn:0219,0618
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
