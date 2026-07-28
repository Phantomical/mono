# Converted from class/Facades/System.ServiceModel.Primitives/Makefile.

mono_declare_managed(
  NAME       System.ServiceModel.Primitives.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.ServiceModel System.Xml System.IdentityModel
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
