# Converted from class/Mono.Posix/Makefile.

mono_declare_managed(
  NAME       Mono.Posix.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../mono.pub
  REFS       System
  FLAGS      /unsafe /nowarn:0618,612
)
