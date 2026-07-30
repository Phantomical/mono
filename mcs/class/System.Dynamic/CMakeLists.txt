mono_declare_managed(
  NAME       System.Dynamic.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System
  FLAGS      -unsafe -d:CODEPLEX_40 -nowarn:414,169
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
