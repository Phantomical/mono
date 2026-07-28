# Converted from class/Facades/System.Xml.ReaderWriter/Makefile.

mono_declare_managed(
  NAME       System.Xml.ReaderWriter.dll
  PROFILES   build net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System.Xml
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
)
