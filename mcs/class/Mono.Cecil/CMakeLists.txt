# Cecil reads and rewrites IL; cil-stringreplacer and ilasm are built on it.
#
# In the bootstrap profile it compiles against the checked-in reference
# assemblies and lands in tmp/, so it can be loaded by the system mono that
# runs those tools before this tree has a usable BCL.
mono_declare_managed(
  NAME       Mono.Cecil.dll
  PROFILES   build
  REFS       System.Core System
  FLAGS      -d:NET_4_0 -unsafe
  KEYFILE    ../mono.snk
  PACKAGE    none
  TARGET_NET_REFERENCE v4.7
  INTERMEDIATE)

# Everywhere else it is an ordinary library built against the in-tree BCL --
# monolinker, pdb2mdb, mono-api-info and permview all reference it.
mono_declare_managed(
  NAME       Mono.Cecil.dll
  PROFILES   net_4_x
  REFS       System.Core System
  FLAGS      -d:NET_4_0 -unsafe
  KEYFILE    ../mono.snk
  PACKAGE    none)
