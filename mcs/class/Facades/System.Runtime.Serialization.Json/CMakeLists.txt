# Converted from class/Facades/System.Runtime.Serialization.Json/Makefile.

mono_declare_managed(
  NAME       System.Runtime.Serialization.Json.dll
  PROFILES   net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System.Runtime.Serialization
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
