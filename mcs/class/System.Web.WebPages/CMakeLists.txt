mono_declare_managed(
  NAME       System.Web.WebPages.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME System.Web.WebPages.dll
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       Microsoft.CSharp Microsoft.Web.Infrastructure System System.ComponentModel.DataAnnotations System.Configuration System.Core System.Data.Linq System.Web System.Web.WebPages.Deployment System.Web.Razor System.Xml System.Xml.Linq
  FLAGS      /warn:1 /delaysign /d:ASPNETWEBPAGES
  RESOURCE_DEFS
    System.Web.WebPages.Common.CommonResources,../../../external/aspnetwebstack/src/CommonResources.resx
    System.Web.WebPages.Resources.WebPageResources,../../../external/aspnetwebstack/src/System.Web.WebPages/Resources/WebPageResources.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
