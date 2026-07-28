# Converted from class/Facades/System.ComponentModel.Annotations/Makefile.

mono_declare_managed(
  NAME       System.ComponentModel.Annotations.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System.ComponentModel.DataAnnotations
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
