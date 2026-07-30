mono_declare_managed(
  NAME       Microsoft.Build.Utilities.dll
  PROFILES   net_4_x
  OUTPUT_NAME Microsoft.Build.Utilities.v4.0.dll
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-net_4_x
  REFS       System System.Core System.Xml Microsoft.Build.Framework
  TEST_REFS  Microsoft.Build.Engine Microsoft.Build.Framework System System.Core
  RESX       Test/Microsoft.Build.Utilities/Strings.resources
  TEST_FLAGS -resource:Test/Microsoft.Build.Utilities/Strings.resources
)

mono_declare_managed(
  NAME       Microsoft.Build.Utilities.dll
  PROFILES   unityjit
  OUTPUT_NAME Microsoft.Build.Utilities.v4.0.dll
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-unityjit
  REFS       System System.Core System.Xml Microsoft.Build.Framework
  TEST_REFS  Microsoft.Build.Engine Microsoft.Build.Framework System System.Core
  RESX       Test/Microsoft.Build.Utilities/Strings.resources
  TEST_FLAGS -resource:Test/Microsoft.Build.Utilities/Strings.resources
)

mono_declare_managed(
  NAME       Microsoft.Build.Utilities.dll
  PROFILES   xbuild_12
  OUTPUT_NAME Microsoft.Build.Utilities.v12.0.dll
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/12.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_12
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Framework
  TEST_REFS  Microsoft.Build.Engine Microsoft.Build.Framework ../net_4_x/System ../net_4_x/System.Core
  RESX       Test/Microsoft.Build.Utilities/Strings.resources
  TEST_FLAGS -resource:Test/Microsoft.Build.Utilities/Strings.resources
)

mono_declare_managed(
  NAME       Microsoft.Build.Utilities.dll
  PROFILES   xbuild_14
  OUTPUT_NAME Microsoft.Build.Utilities.Core.dll
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/14.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_14
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Framework
  TEST_REFS  Microsoft.Build.Engine Microsoft.Build.Framework ../net_4_x/System ../net_4_x/System.Core
  RESX       Test/Microsoft.Build.Utilities/Strings.resources
  TEST_FLAGS -resource:Test/Microsoft.Build.Utilities/Strings.resources
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
