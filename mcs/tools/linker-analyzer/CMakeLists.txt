# Converted from tools/linker-analyzer/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       illinkanalyzer.exe
  PROFILES   net_4_x
  REFS       System System.Xml System.Core Mono.Cecil
)
