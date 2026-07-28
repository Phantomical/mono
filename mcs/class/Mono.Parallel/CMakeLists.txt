# Converted from class/Mono.Parallel/Makefile.

mono_declare_managed(
  NAME       Mono.Parallel.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  REFS       System.Core System
  FLAGS      -d:INSIDE_MONO_PARALLEL
)
