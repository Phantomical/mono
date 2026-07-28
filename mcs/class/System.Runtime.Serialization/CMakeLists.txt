# Converted from class/System.Runtime.Serialization/Makefile.

mono_declare_managed(
  NAME       System.Runtime.Serialization.dll
  PROFILES   net_4_x unityjit
  REFS       System System.Xml System.Core System.ServiceModel.Internals System.Data System.Configuration SMDiagnostics
  FLAGS      -unsafe -d:NO_DYNAMIC_CODEGEN /nowarn:168,169,219,414,618,1634 /d:NET_3_0
)
