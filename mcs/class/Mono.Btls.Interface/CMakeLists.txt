mono_declare_managed(
  NAME       Mono.Btls.Interface.dll
  PROFILES   net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  REFS       System Mono.Security
  FLAGS      -unsafe -nowarn:1030 -keyfile:../mono.pub -delaysign -d:SECURITY_DEP
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
