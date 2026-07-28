# Converted from class/System.Net.Http/Makefile.

mono_declare_managed(
  NAME       System.Net.Http.dll
  PROFILES   build net_4_x
  KEYFILE    ../msfinal.pub
  REFS       System.Core System
  FLAGS      -unsafe -nowarn:436
)

mono_declare_managed(
  NAME       System.Net.Http.dll
  PROFILES   unityjit
  KEYFILE    ../msfinal.pub
  REFS       System.Core System Mono.Security
  FLAGS      -unsafe -nowarn:436 -d:SECURITY_DEP
)
