mono_declare_managed(
  PROGRAM
  NAME       commoncryptogenerator.exe
  PROFILES   build
  NO_INSTALL
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
