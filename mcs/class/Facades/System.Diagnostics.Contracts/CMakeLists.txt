# Converted from class/Facades/System.Diagnostics.Contracts/Makefile.

mono_declare_managed(
  NAME       System.Diagnostics.Contracts.dll
  PROFILES   build net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
