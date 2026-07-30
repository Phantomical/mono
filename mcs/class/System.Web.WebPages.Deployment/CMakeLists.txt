mono_declare_managed(
  NAME       System.Web.WebPages.Deployment.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME System.Web.WebPages.Deployment.dll
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Configuration System.Web Microsoft.Web.Infrastructure
  FLAGS      /warn:1 -delaysign /d:ASPNETWEBPAGES
  RESOURCE_DEFS
    System.Web.WebPages.Deployment.Common.CommonResources,../../../external/aspnetwebstack/src/CommonResources.resx
    System.Web.WebPages.Deployment.Resources.ConfigurationResources,../../../external/aspnetwebstack/src/System.Web.WebPages.Deployment/Resources/ConfigurationResources.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
