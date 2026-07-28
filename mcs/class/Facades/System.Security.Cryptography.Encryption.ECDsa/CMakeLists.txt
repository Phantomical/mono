# Converted from class/Facades/System.Security.Cryptography.Encryption.ECDsa/Makefile.

mono_declare_managed(
  NAME       System.Security.Cryptography.Encryption.ECDsa.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.Core
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
