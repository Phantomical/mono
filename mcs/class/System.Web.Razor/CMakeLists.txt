mono_declare_managed(
  NAME       System.Web.Razor.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME System.Web.Razor.dll
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core
  FLAGS      /warn:1 -delaysign /d:ASPNETWEBPAGES
  RESOURCE_DEFS
    System.Web.Razor.Common.CommonResources,../../../external/aspnetwebstack/src/CommonResources.resx
    System.Web.Razor.Resources.RazorResources,../../../external/aspnetwebstack/src/System.Web.Razor/Resources/RazorResources.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
