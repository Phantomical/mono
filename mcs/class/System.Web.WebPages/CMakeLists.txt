# Converted from class/System.Web.WebPages/Makefile.

mono_declare_managed(
  NAME       System.Web.WebPages.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME System.Web.WebPages.dll
  KEYFILE    ../winfx.pub
  REFS       Microsoft.CSharp Microsoft.Web.Infrastructure System System.ComponentModel.DataAnnotations System.Configuration System.Core System.Data.Linq System.Web System.Web.WebPages.Deployment System.Web.Razor System.Xml System.Xml.Linq
  FLAGS      /warn:1 /delaysign /d:ASPNETWEBPAGES
)
