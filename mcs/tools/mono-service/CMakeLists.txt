# Converted from tools/mono-service/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       mono-service.exe
  PROFILES   net_4_x
  SNK        ../../class/mono.snk
  REFS       System.ServiceProcess Mono.Posix System
  FLAGS      -unsafe -publicsign -keyfile:../../class/mono.snk
)
