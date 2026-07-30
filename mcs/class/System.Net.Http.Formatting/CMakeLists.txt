mono_declare_managed(
  NAME       System.Net.Http.Formatting.dll
  PROFILES   net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System System.Net.Http System.Xml System.Runtime.Serialization System.Xml.Linq System.Data System.Configuration
  FLAGS      -d:ASPNETMVC -keyfile:../winfx.pub -delaysign
  RESOURCE_DEFS
    System.Net.Http.Properties.CommonWebApiResources,../../../external/aspnetwebstack/src/Common/CommonWebApiResources.resx
    System.Net.Http.Properties.Resources,../../../external/aspnetwebstack/src/System.Net.Http.Formatting/Properties/Resources.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
