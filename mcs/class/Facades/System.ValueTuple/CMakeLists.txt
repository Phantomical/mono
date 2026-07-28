# Converted from class/Facades/System.ValueTuple/Makefile.

mono_declare_managed(
  NAME       System.ValueTuple.dll
  PROFILES   build net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../Open.snk
  INSTALL_DIR lib/mono/4.5/Facades
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
