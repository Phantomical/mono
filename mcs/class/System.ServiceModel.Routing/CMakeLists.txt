# Converted from class/System.ServiceModel.Routing/Makefile.

mono_declare_managed(
  NAME       System.ServiceModel.Routing.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  REFS       System System.Xml System.Runtime.Serialization System.ServiceModel System.Core System.Configuration
  FLAGS      -d:NET_3_5 -d:NET_3_0
)
