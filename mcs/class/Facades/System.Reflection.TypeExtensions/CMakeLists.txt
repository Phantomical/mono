# Converted from class/Facades/System.Reflection.TypeExtensions/Makefile.

mono_declare_managed(
  NAME       System.Reflection.TypeExtensions.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System
  FLAGS      /delaysign /nowarn:1616,1699
)
