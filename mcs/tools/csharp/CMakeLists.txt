mono_declare_managed(
  PROGRAM
  NAME       csharp.exe
  PROFILES   net_4_x
  REFS       Mono.CSharp Mono.Posix Mono.Management System
  FLAGS      -unsafe -nowarn:3021
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
