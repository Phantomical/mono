# Converted from class/System.ServiceModel/Makefile.

mono_declare_managed(
  NAME       System.ServiceModel.dll
  PROFILES   net_4_x unityjit
  REFS       System System.Xml System.Core System.Runtime.Serialization System.IdentityModel System.Configuration System.Data System.Security System.IdentityModel.Selectors System.Transactions System.Messaging System.Web.Services Mono.Security System.Web System.Web.ApplicationServices
  API_BIN_REFS System.ServiceModel.Activation
  FLAGS      /nowarn:414,169,67,3005,436,219,618 /unsafe /d:TRACE /resource:resources/WS-Addressing.schema /resource:resources/ws-addr.xsd /d:NET_3_0 /d:SERVICEMODEL_ACTIVATION_DEPENDENCY
)
