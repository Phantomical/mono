mono_declare_managed(
  NAME       System.Net.dll
  PROFILES   net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Xml
  FLAGS      -unsafe -d:NET_3_5 -nowarn:1720
  TEST_FLAGS -unsafe -d:NET_3_5 -nowarn:1720
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
