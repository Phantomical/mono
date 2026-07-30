mono_declare_managed(
  NAME       System.ServiceModel.Web.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Runtime.Serialization System.ServiceModel System.Core System.Configuration System.Web.Extensions System.ServiceModel.Activation
  FLAGS      -d:NET_3_5 -d:NET_3_0
  TEST_FLAGS -d:NET_3_5 -d:NET_3_0 -resource:Test/config/webHttpBinding,Test/config/webHttpBinding -resource:Test/config/webHttpBinding.config,Test/config/webHttpBinding.config
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
