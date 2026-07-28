# Converted from class/Facades/System.Security.Cryptography.RSA/Makefile.

mono_declare_managed(
  NAME       System.Security.Cryptography.RSA.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
