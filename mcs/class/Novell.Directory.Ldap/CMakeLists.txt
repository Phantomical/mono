# Converted from class/Novell.Directory.Ldap/Makefile.

mono_declare_managed(
  NAME       Novell.Directory.Ldap.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  REFS       System Mono.Security
  FLAGS      -warn:1 -nowarn:612
)
