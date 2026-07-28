# Converted from class/Facades/System.ServiceModel.NetTcp/Makefile.

mono_declare_managed(
  NAME       System.ServiceModel.NetTcp.dll
  PROFILES   net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System.ServiceModel
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
