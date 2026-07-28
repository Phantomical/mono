# Converted from class/Facades/System.Security.Cryptography.Pkcs/Makefile.

mono_declare_managed(
  NAME       System.Security.Cryptography.Pkcs.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.Security
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
