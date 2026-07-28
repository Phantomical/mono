# Converted from class/System.IO.Compression.FileSystem/Makefile.

mono_declare_managed(
  NAME       System.IO.Compression.FileSystem.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System System.IO.Compression
  FLAGS      -unsafe -nowarn:436
)
