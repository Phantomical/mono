mono_declare_managed(
  PROGRAM
  NAME       culevel.exe
  PROFILES   build net_4_x
  REFS       System System.Xml
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
