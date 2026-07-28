# Converted from class/System.Runtime.Caching/Makefile.

mono_declare_managed(
  NAME       System.Runtime.Caching.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME dummy-System.Runtime.Caching.dll
  KEYFILE    ../msfinal.pub
  REFS       System System.Data System.Configuration System.Core
  FLAGS      -nowarn:414
  NO_INSTALL
  NO_SIGN
)
