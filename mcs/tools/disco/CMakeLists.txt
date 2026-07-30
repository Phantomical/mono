mono_declare_managed(
  PROGRAM
  NAME       disco.exe
  PROFILES   net_4_x
  REFS       System.Xml System.Web.Services System
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
