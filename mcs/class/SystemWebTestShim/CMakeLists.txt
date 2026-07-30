mono_declare_managed(
  NAME       SystemWebTestShim.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Web System
  NO_INSTALL
  NO_SIGN
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
