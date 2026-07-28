# Converted from class/System.Web.WebPages.Deployment/Makefile.

mono_declare_managed(
  NAME       System.Web.WebPages.Deployment.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME System.Web.WebPages.Deployment.dll
  KEYFILE    ../winfx.pub
  REFS       System System.Core System.Configuration System.Web Microsoft.Web.Infrastructure
  FLAGS      /warn:1 -delaysign /d:ASPNETWEBPAGES
)
