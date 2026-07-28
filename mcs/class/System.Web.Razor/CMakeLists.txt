# Converted from class/System.Web.Razor/Makefile.

mono_declare_managed(
  NAME       System.Web.Razor.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME System.Web.Razor.dll
  KEYFILE    ../winfx.pub
  REFS       System System.Core
  FLAGS      /warn:1 -delaysign /d:ASPNETWEBPAGES
)
