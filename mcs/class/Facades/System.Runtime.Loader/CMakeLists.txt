# Converted from class/Facades/System.Runtime.Loader/Makefile.

mono_declare_managed(
  NAME       System.Runtime.Loader.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System
  FLAGS      /delaysign /nowarn:1616,1699 /unsafe
)
