# Converted from class/System.Web.Mvc3/Makefile.

mono_declare_managed(
  NAME       System.Web.Mvc3.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME System.Web.Mvc.dll
  KEYFILE    ../winfx.pub
  REFS       Microsoft.Web.Infrastructure System System.Core System.Configuration System.Data System.Xml System.Web System.Web.Abstractions System.Web.Routing System.Web.Extensions System.ComponentModel.DataAnnotations System.Data.Linq System.Runtime.Caching System.Web.Razor System.Web.WebPages.Razor System.Web.WebPages
  FLAGS      /warn:1 /d:MONO /delaysign
)
