# Converted from class/Facades/System.ValueTuple/Makefile.

mono_declare_managed(
  NAME       System.ValueTuple.dll
  PROFILES   build net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../Open.snk
  INSTALL_DIR /mono//Facades
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
