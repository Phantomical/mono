# Converted from class/Facades/System.Xml.XDocument/Makefile.

mono_declare_managed(
  NAME       System.Xml.XDocument.dll
  PROFILES   build net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System.Xml.Linq System.Xml
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
