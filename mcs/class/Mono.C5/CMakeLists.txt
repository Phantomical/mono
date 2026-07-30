mono_declare_managed(
  NAME       Mono.C5.dll
  PROFILES   net_4_x unityjit
  KEYFILE    c5.pub
  SNK        c5.snk
  REFS       System
  FLAGS      -nowarn:169,219,414,1030,3001,3005,3006
  TEST_FLAGS -nowarn:169,219,414,1030,3001,3005,3006 -nowarn:0618 -nowarn:219 -nowarn:169
  NO_INSTALL
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
