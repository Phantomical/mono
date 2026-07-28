# Converted from class/System.Threading.Tasks.Dataflow/Makefile.

mono_declare_managed(
  NAME       System.Threading.Tasks.Dataflow.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System.Core System
  FLAGS      -d:CONCURRENT_COLLECTIONS
)
