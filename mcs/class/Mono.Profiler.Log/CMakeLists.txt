# Converted from class/Mono.Profiler.Log/Makefile.

mono_declare_managed(
  NAME       Mono.Profiler.Log.dll
  PROFILES   net_4_x unityjit
  REFS       System System.Core
  FLAGS      /unsafe /publicsign /nowarn:0618 -D:MONO_DATACONVERTER_STATIC_METHOD
)
