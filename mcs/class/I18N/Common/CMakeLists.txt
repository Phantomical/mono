mono_declare_managed(
  NAME       I18N.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../../mono.pub
  SNK        <topdir>/class/mono.snk
  FLAGS      /unsafe /define:DISABLE_UNSAFE
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
