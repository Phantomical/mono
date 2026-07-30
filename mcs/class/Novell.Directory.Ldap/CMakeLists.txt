mono_declare_managed(
  NAME       Novell.Directory.Ldap.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System Mono.Security
  FLAGS      -warn:1 -nowarn:612
  RESOURCE_DEFS ResultCodeMessages,Novell.Directory.Ldap.Utilclass/ResultCodeMessages.txt
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
