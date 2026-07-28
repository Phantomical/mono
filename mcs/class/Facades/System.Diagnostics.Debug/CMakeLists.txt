# Converted from class/Facades/System.Diagnostics.Debug/Makefile.

mono_declare_managed(
  NAME       System.Diagnostics.Debug.dll
  PROFILES   build net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
