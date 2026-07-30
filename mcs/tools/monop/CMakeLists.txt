mono_declare_managed(
  PROGRAM
  NAME       monop.exe
  PROFILES   net_4_x
  REFS       System
  FLAGS      -d:NO_AUTHENTICODE,STATIC,NO_SYMBOL_WRITER
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
