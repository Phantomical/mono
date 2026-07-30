mono_declare_managed(
  NAME       Mono.Security.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  API_BIN_REFS System
  FLAGS      -unsafe -nowarn:1030,3009
  TEST_FLAGS -unsafe -nowarn:1030,3009 -nowarn:169,219,618,672
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
