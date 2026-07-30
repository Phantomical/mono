mono_declare_managed(
  NAME       Mono.WebBrowser.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.snk
  SNK        <topdir>/class/mono.snk
  REFS       System
  FLAGS      -warn:1
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
