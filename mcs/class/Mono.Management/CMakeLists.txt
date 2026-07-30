mono_declare_managed(
  NAME       Mono.Management.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System Mono.Posix
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
