# Converted from class/Facades/System.Reflection.DispatchProxy/Makefile.

mono_declare_managed(
  NAME       System.Reflection.DispatchProxy.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System System.Core
  FLAGS      /delaysign /nowarn:1616,1699 /unsafe
)
