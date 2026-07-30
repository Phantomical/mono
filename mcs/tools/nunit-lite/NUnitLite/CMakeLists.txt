# The test framework every class-library test assembly links against.
mono_declare_managed(
  NAME       nunitlite.dll
  PROFILES   net_4_x
  KEYFILE    ../../../class/mono.snk
  REFS       System System.Xml System.Core
  FLAGS      -d:__MOBILE__ -d:TRACE -d:DEBUG -d:NET_4_0 -d:CLR_4_0,NUNITLITE
             /warn:4 /delaysign
  NO_INSTALL)
