mono_declare_managed(
  NAME       System.Web.Http.WebHost.dll
  PROFILES   net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System System.Xml System.Configuration System.Net.Http System.Runtime.Serialization System.ServiceModel System.IdentityModel System.Web.Http System.Net.Http.Formatting System.Web.Routing System.Web Microsoft.Web.Infrastructure
  FLAGS      -d:ASPNETMVC -keyfile:../winfx.pub -delaysign
  RESOURCE_DEFS
    System.Web.Http.WebHost.Properties.CommonWebApiResources,../../../external/aspnetwebstack/src/Common/CommonWebApiResources.resx
    System.Web.Http.WebHost.Properties.SRResources,../../../external/aspnetwebstack/src/System.Web.Http.WebHost/Properties/SRResources.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
