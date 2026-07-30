mono_declare_managed(
  PROGRAM
  NAME       resx2sr.exe
  PROFILES   build
  REFS       System System.Drawing System.Xml
  FLAGS      -unsafe
  NO_INSTALL
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
