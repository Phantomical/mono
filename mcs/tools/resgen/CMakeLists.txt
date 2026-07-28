# Converted from tools/resgen/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       resgen.exe
  PROFILES   build net_4_x
  REFS       System System.Xml System.Core System.Drawing
  FLAGS      -unsafe
  NO_INSTALL
)
