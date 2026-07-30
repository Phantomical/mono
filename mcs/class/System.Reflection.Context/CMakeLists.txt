mono_declare_managed(
  NAME       System.Reflection.Context.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
