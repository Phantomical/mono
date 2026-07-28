# Converted from class/Facades/System.Security.Cryptography.Cng/Makefile.

mono_declare_managed(
  NAME       System.Security.Cryptography.Cng.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.Core
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
