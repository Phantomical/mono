mono_declare_managed(
  PROGRAM
  NAME       sqlsharp.exe
  PROFILES   net_4_x
  REFS       System System.Xml System.Data
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
