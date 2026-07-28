# Splices a resource string table, and an ilasm'd IL module, into an assembly
# that csc has already emitted.  corlib is the only consumer.
mono_declare_managed(
  PROGRAM
  NAME       cil-stringreplacer.exe
  PROFILES   build
  REFS       System Mono.Cecil
  TARGET_NET_REFERENCE v4.7
  INTERMEDIATE
  NO_INSTALL)
