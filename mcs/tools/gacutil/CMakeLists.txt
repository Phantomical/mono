mono_declare_managed(
  PROGRAM
  NAME       gacutil.exe
  PROFILES   build net_4_x
  REFS       System Mono.Security System.Security
  FLAGS      -unsafe -define:NO_SYMBOL_WRITER
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
