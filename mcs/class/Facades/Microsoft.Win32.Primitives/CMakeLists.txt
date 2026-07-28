# Converted from class/Facades/Microsoft.Win32.Primitives/Makefile.

mono_declare_managed(
  NAME       Microsoft.Win32.Primitives.dll
  PROFILES   net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
