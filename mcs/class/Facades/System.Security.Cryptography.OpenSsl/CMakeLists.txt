# Converted from class/Facades/System.Security.Cryptography.OpenSsl/Makefile.

mono_declare_managed(
  NAME       System.Security.Cryptography.OpenSsl.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System.Core
  FLAGS      /delaysign /nowarn:1616,1699
)
