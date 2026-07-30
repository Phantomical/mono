mono_declare_managed(
  PROGRAM
  NAME       mono-symbolicate.exe
  PROFILES   net_4_x
  REFS       System.Xml System.Core System
  FLAGS      /D:NO_AUTHENTICODE /D:CECIL -nowarn:649 -unsafe
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
