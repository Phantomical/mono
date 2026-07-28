# The IL linker.
#
# Hand-written: the Makefile's recipes are the linker's own regression suite,
# which is separate from building the tool.
mono_declare_managed(
  PROGRAM
  NAME       monolinker.exe
  PROFILES   build net_4_x
  REFS       System System.Core System.Xml Mono.Cecil
  FLAGS      -unsafe
  NO_INSTALL)
