mono_declare_managed(
  PROGRAM
  NAME       mono-api-html.exe
  PROFILES   build net_4_x
  REFS       System.Xml System.Core System System.Xml.Linq
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
