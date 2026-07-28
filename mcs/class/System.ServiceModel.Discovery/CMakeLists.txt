# Converted from class/System.ServiceModel.Discovery/Makefile.

mono_declare_managed(
  NAME       System.ServiceModel.Discovery.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  REFS       System System.Xml System.Runtime.Serialization System.ServiceModel System.Core System.Xml.Linq System.Configuration
  FLAGS      -d:NET_3_5 -d:NET_3_0
)
