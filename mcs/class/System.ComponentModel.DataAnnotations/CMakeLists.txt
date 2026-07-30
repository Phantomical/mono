mono_declare_managed(
  NAME       System.ComponentModel.DataAnnotations.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Xml
  FLAGS      -nowarn:414
  TEST_REFS  System System.Core
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
