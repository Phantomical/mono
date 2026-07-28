# Converted from class/System.Web.Http/Makefile.

mono_declare_managed(
  NAME       System.Web.Http.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  REFS       System.Core System System.Xml System.Net.Http System.ComponentModel.DataAnnotations System.Net.Http.Formatting System.Runtime.Caching System.Runtime.Serialization System.Data.Linq
  FLAGS      -d:ASPNETMVC -delaysign
)
