# Converted from class/Facades/System.Linq.Expressions/Makefile.

mono_declare_managed(
  NAME       System.Linq.Expressions.dll
  PROFILES   build net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System.Core
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
