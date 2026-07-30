mono_declare_managed(
  NAME       Mono.Options.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    none
  REFS       System
  TEST_REFS  Mono.Posix System System.Core
  NO_INSTALL
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
