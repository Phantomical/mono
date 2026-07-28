# Converted from class/System.Json/Makefile.

mono_declare_managed(
  NAME       System.Json.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  REFS       System System.Xml System.Core
  FLAGS      -nowarn:3021
)
