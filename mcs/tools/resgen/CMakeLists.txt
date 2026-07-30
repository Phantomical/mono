mono_declare_managed(
  PROGRAM
  NAME       resgen.exe
  PROFILES   build
  REFS       System System.Xml System.Core System.Drawing
  FLAGS      -unsafe
  NO_INSTALL
)

mono_declare_managed(
  PROGRAM
  NAME       resgen.exe
  PROFILES   net_4_x
  REFS       System System.Xml System.Core System.Drawing
  FLAGS      -unsafe
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
