# Converted from tools/resx2sr/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       resx2sr.exe
  PROFILES   build
  REFS       System System.Drawing System.Xml
  FLAGS      -unsafe
  NO_INSTALL
)
