# Converted from class/System.Security/Makefile.

mono_declare_managed(
  NAME       System.Security.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       Mono.Security System System.Xml
  API_BIN_REFS System.Numerics System.Core
  FLAGS      -unsafe -nowarn:414,618 -d:SECURITY_DEP
)
