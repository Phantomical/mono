mono_declare_managed(
  NAME       System.Runtime.Caching.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Data System.Configuration System.Core
  FLAGS      -nowarn:414
  TEST_FLAGS -nowarn:414
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
