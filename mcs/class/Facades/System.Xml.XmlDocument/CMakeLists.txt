# Converted from class/Facades/System.Xml.XmlDocument/Makefile.

mono_declare_managed(
  NAME       System.Xml.XmlDocument.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR /mono//Facades
  REFS       System System.Xml
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
