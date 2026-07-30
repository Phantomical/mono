mono_declare_managed(
  PROGRAM
  NAME       mono-service.exe
  PROFILES   net_4_x
  SNK        ../../class/mono.snk
  REFS       System.ServiceProcess Mono.Posix System
  FLAGS      -unsafe -publicsign -keyfile:../../class/mono.snk
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
