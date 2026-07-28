# Converted from class/Facades/System.Reflection.Extensions/Makefile.

mono_declare_managed(
  NAME       System.Reflection.Extensions.dll
  PROFILES   build net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
