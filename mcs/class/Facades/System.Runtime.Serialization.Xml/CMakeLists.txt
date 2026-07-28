# Converted from class/Facades/System.Runtime.Serialization.Xml/Makefile.

mono_declare_managed(
  NAME       System.Runtime.Serialization.Xml.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.Runtime.Serialization System.Xml
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
