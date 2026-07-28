# Converted from class/System.Dynamic/Makefile.

mono_declare_managed(
  NAME       System.Dynamic.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System.Core System
  FLAGS      -unsafe -d:CODEPLEX_40 -nowarn:414,169
)
