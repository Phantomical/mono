# Converted from class/System.Web.Http.SelfHost/Makefile.

mono_declare_managed(
  NAME       System.Web.Http.SelfHost.dll
  PROFILES   net_4_x unityjit
  REFS       System.Core System System.Xml System.Configuration System.Net.Http System.Runtime.Serialization System.ServiceModel System.IdentityModel System.Web.Http System.Net.Http.Formatting
  FLAGS      -d:ASPNETMVC -keyfile:../winfx.pub -delaysign
)
