mono_declare_managed(
  NAME       Mono.Tasklets.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
