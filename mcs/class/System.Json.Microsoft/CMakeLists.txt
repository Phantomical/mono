mono_declare_managed(
  NAME       System.Json.Microsoft.dll
  PROFILES   net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Core System.Runtime.Serialization Microsoft.CSharp
  FLAGS      /d:ASPNETMVC -keyfile:../winfx.pub -delaysign -d:FEATURE_DYNAMIC
  TEST_REFS  System.Core
  RESOURCE_DEFS System.Json.Properties.Resources,System.Json/Properties/Resources.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
