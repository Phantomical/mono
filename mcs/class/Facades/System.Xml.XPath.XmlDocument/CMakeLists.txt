mono_declare_managed(
  NAME       System.Xml.XPath.XmlDocument.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  SNK        <topdir>/class/mono.snk
  INSTALL_DIR <libdir>/mono/4.5/Facades
  REFS       System System.Xml
  FLAGS      /delaysign /nowarn:1616,1699
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
