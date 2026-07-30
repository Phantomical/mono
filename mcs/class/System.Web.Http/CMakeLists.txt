mono_declare_managed(
  NAME       System.Web.Http.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System System.Xml System.Net.Http System.ComponentModel.DataAnnotations System.Net.Http.Formatting System.Runtime.Caching System.Runtime.Serialization System.Data.Linq
  FLAGS      -d:ASPNETMVC -delaysign
  RESOURCE_DEFS
    System.Web.Http.Properties.CommonWebApiResources,../../../external/aspnetwebstack/src/Common/CommonWebApiResources.resx
    System.Web.Http.Properties.SRResources,../../../external/aspnetwebstack/src/System.Web.Http/Properties/SRResources.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
