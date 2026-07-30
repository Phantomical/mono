mono_declare_managed(
  NAME       Microsoft.Build.Engine.dll
  PROFILES   net_4_x
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-net_4_x
  REFS       System System.Core System.Xml Microsoft.Build.Framework Microsoft.Build.Utilities.v4.0
  TEST_REFS  Microsoft.Build.Framework Microsoft.Build.Utilities.v4.0 System.Xml
  TEST_FLAGS -resource:Test/resources/Import.csproj,Test/resources/Import.csproj -resource:Test/resources/Items.csproj,Test/resources/Items.csproj -resource:Test/resources/RelativeImport1.csproj,Test/resources/RelativeImport1.csproj -resource:Test/resources/RelativeImport2.csproj,Test/resources/RelativeImport2.csproj -resource:Test/resources/SelfImport.csproj,Test/resources/SelfImport.csproj -resource:Test/resources/TestReturns.csproj,Test/resources/TestReturns.csproj -resource:Test/resources/TestTasks-net_4_x.dll,Test/resources/TestTasks.dll -resource:Test/resources/TestTasks-net_4_x.pdb,Test/resources/TestTasks.pdb
)

mono_declare_managed(
  NAME       Microsoft.Build.Engine.dll
  PROFILES   unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-unityjit
  REFS       System System.Core System.Xml Microsoft.Build.Framework Microsoft.Build.Utilities.v4.0
  TEST_REFS  Microsoft.Build.Framework Microsoft.Build.Utilities.v4.0 System.Xml
  TEST_FLAGS -resource:Test/resources/Import.csproj,Test/resources/Import.csproj -resource:Test/resources/Items.csproj,Test/resources/Items.csproj -resource:Test/resources/RelativeImport1.csproj,Test/resources/RelativeImport1.csproj -resource:Test/resources/RelativeImport2.csproj,Test/resources/RelativeImport2.csproj -resource:Test/resources/SelfImport.csproj,Test/resources/SelfImport.csproj -resource:Test/resources/TestReturns.csproj,Test/resources/TestReturns.csproj -resource:Test/resources/TestTasks-unityjit.dll,Test/resources/TestTasks.dll -resource:Test/resources/TestTasks-unityjit.pdb,Test/resources/TestTasks.pdb
)

mono_declare_managed(
  NAME       Microsoft.Build.Engine.dll
  PROFILES   xbuild_12
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/12.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_12
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Framework Microsoft.Build.Utilities.v12.0
  TEST_REFS  Microsoft.Build.Framework Microsoft.Build.Utilities.v12.0 ../net_4_x/System.Xml
  TEST_FLAGS -resource:Test/resources/Import.csproj,Test/resources/Import.csproj -resource:Test/resources/Items.csproj,Test/resources/Items.csproj -resource:Test/resources/RelativeImport1.csproj,Test/resources/RelativeImport1.csproj -resource:Test/resources/RelativeImport2.csproj,Test/resources/RelativeImport2.csproj -resource:Test/resources/SelfImport.csproj,Test/resources/SelfImport.csproj -resource:Test/resources/TestReturns.csproj,Test/resources/TestReturns.csproj -resource:Test/resources/TestTasks-xbuild_12.dll,Test/resources/TestTasks.dll -resource:Test/resources/TestTasks-xbuild_12.pdb,Test/resources/TestTasks.pdb
)

mono_declare_managed(
  NAME       Microsoft.Build.Engine.dll
  PROFILES   xbuild_14
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/14.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_14
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Framework Microsoft.Build.Utilities.Core
  TEST_REFS  Microsoft.Build.Framework Microsoft.Build.Utilities.Core ../net_4_x/System.Xml
  TEST_FLAGS -resource:Test/resources/Import.csproj,Test/resources/Import.csproj -resource:Test/resources/Items.csproj,Test/resources/Items.csproj -resource:Test/resources/RelativeImport1.csproj,Test/resources/RelativeImport1.csproj -resource:Test/resources/RelativeImport2.csproj,Test/resources/RelativeImport2.csproj -resource:Test/resources/SelfImport.csproj,Test/resources/SelfImport.csproj -resource:Test/resources/TestReturns.csproj,Test/resources/TestReturns.csproj -resource:Test/resources/TestTasks-xbuild_14.dll,Test/resources/TestTasks.dll -resource:Test/resources/TestTasks-xbuild_14.pdb,Test/resources/TestTasks.pdb
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
