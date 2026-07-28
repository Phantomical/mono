# Converted from class/Facades/System.Runtime.Loader/Makefile.

mono_declare_managed(
  NAME       System.Runtime.Loader.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System
  FLAGS      /delaysign /nowarn:1616,1699 /unsafe
)
