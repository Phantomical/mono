# Converted from class/Facades/System.Runtime/Makefile.

mono_declare_managed(
  NAME       System.Runtime.dll
  PROFILES   build net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.ComponentModel.Composition System.Core
  FLAGS      /delaysign /nowarn:1616,1699,618
  NO_DEBUG
)
