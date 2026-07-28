# Converted from class/Mono.CSharp/Makefile.

mono_declare_managed(
  NAME       Mono.CSharp.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  REFS       System.Core System.Xml System
  FLAGS      -d:MONO_FEATURE_THREAD_ABORT -d:MONO_FEATURE_PROCESS_START
)
