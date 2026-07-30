mono_declare_managed(
  NAME       System.Web.WebPages.Razor.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME System.Web.WebPages.Razor.dll
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Configuration System.Web System.Web.WebPages System.Web.Razor
  FLAGS      /warn:1 /delaysign /d:ASPNETWEBPAGES
  RESOURCE_DEFS
    System.Web.WebPages.Razor.Common.CommonResources,../../../external/aspnetwebstack/src/CommonResources.resx
    System.Web.WebPages.Razor.Resources.RazorWebResources,../../../external/aspnetwebstack/src/System.Web.WebPages.Razor/Resources/RazorWebResources.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
