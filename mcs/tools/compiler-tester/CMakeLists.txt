mono_declare_managed(
  PROGRAM
  NAME       compiler-tester.exe
  PROFILES   net_4_x
  REFS       System System.Core System.Xml
  NO_INSTALL
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
