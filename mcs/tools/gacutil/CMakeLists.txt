# Converted from tools/gacutil/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       gacutil.exe
  PROFILES   build net_4_x
  REFS       System Mono.Security System.Security
  FLAGS      -unsafe -define:NO_SYMBOL_WRITER
)
