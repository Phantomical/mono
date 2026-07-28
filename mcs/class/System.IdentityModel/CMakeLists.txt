# Converted from class/System.IdentityModel/Makefile.

mono_declare_managed(
  NAME       System.IdentityModel.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System System.Xml System.Security Mono.Security System.Runtime.Serialization System.Configuration System.Web
  FLAGS      /d:NET_3_0
)
