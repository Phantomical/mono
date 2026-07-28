# Converted from class/Facades/System.Reflection.DispatchProxy/Makefile.

mono_declare_managed(
  NAME       System.Reflection.DispatchProxy.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.Core
  FLAGS      /delaysign /nowarn:1616,1699 /unsafe
)
