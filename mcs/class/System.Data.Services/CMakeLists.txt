# Converted from class/System.Data.Services/Makefile.

mono_declare_managed(
  NAME       System.Data.Services.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System System.Core System.ServiceModel System.ServiceModel.Web System.Data.Services.Client
  FLAGS      -d:NET_3_5
)
