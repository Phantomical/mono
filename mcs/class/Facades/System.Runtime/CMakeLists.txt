# Converted from class/Facades/System.Runtime/Makefile.

mono_declare_managed(
  NAME       System.Runtime.dll
  PROFILES   build net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System System.ComponentModel.Composition System.Core
  FLAGS      /delaysign /nowarn:1616,1699,618
  NO_DEBUG
)
