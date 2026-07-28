# Converted from class/Facades/System.Security.Cryptography.Encryption.ECDsa/Makefile.

mono_declare_managed(
  NAME       System.Security.Cryptography.Encryption.ECDsa.dll
  PROFILES   net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System System.Core
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
