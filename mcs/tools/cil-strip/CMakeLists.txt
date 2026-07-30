mono_declare_managed(
  PROGRAM
  NAME       mono-cil-strip.exe
  PROFILES   build net_4_x
  REFS       System
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
