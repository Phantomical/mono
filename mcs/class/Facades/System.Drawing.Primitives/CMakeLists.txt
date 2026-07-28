# Converted from class/Facades/System.Drawing.Primitives/Makefile.

mono_declare_managed(
  NAME       System.Drawing.Primitives.dll
  PROFILES   net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.Drawing
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)

mono_declare_managed(
  NAME       System.Drawing.Primitives.dll
  PROFILES   xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System Facades/System.Drawing.Common
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
