mono_declare_managed(
  NAME       System.Web.Mvc3.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME System.Web.Mvc.dll
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       Microsoft.Web.Infrastructure System System.Core System.Configuration System.Data System.Xml System.Web System.Web.Abstractions System.Web.Routing System.Web.Extensions System.ComponentModel.DataAnnotations System.Data.Linq System.Runtime.Caching System.Web.Razor System.Web.WebPages.Razor System.Web.WebPages
  FLAGS      /warn:1 /d:MONO /delaysign
  RESOURCE_DEFS System.Web.Mvc.Resources.MvcResources,Mvc/Resources/MvcResources.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
