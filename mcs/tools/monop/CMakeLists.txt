# Converted from tools/monop/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       monop.exe
  PROFILES   net_4_x
  REFS       System
  FLAGS      -d:NO_AUTHENTICODE,STATIC,NO_SYMBOL_WRITER
)
