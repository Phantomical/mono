# Converted from class/Facades/System.IO.Compression.ZipFile/Makefile.

mono_declare_managed(
  NAME       System.IO.Compression.ZipFile.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../ecma.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.IO.Compression.FileSystem
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
