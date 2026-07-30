mono_declare_managed(
  NAME       System.IdentityModel.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Security Mono.Security System.Runtime.Serialization System.Configuration System.Web System.Web.ApplicationServices
  FLAGS      /d:NET_3_0
  TEST_REFS  System System.Xml System.Security System.Runtime.Serialization
  TEST_FLAGS -resource:Test/Resources/test.cer,Test/Resources/test.cer -resource:Test/Resources/test_neg_serial.cer,Test/Resources/test_neg_serial.cer -resource:Test/Resources/test.pfx,Test/Resources/test.pfx -resource:Test/Resources/test2.pfx,Test/Resources/test2.pfx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
