mono_declare_managed(
  PROGRAM
  NAME       macpack.exe
  PROFILES   net_4_x
  FLAGS      /resource:LOADER /resource:PLIST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
