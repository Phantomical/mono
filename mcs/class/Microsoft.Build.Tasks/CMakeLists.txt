mono_declare_managed(
  NAME       Microsoft.Build.Tasks.dll
  PROFILES   net_4_x
  OUTPUT_NAME Microsoft.Build.Tasks.v4.0.dll
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-net_4_x
  REFS       System System.Core System.Xml System.Windows.Forms Microsoft.Build.Utilities.v4.0 Microsoft.Build.Framework Microsoft.Build.Engine Mono.XBuild.Tasks
  TEST_REFS  System.Xml Microsoft.Build.Engine Microsoft.Build.Framework Mono.XBuild.Tasks Microsoft.Build.Utilities.v4.0 System.Core Microsoft.Build
  TEST_FLAGS -resource:Test/resources/test.cs,Test/resources/test.cs -resource:Test/resources/test.dll,Test/resources/test.dll -resource:Test/resources/Sample.cs,Test/resources/Sample.cs -resource:Test/resources/Sample.vb,Test/resources/Sample.vb -resource:Test/resources/junk.txt,Test/resources/junk.txt -resource:Test/resources/binary/FancyStuff.dll,Test/resources/binary/FancyStuff.dll -resource:Test/resources/binary/FancyStuff.dll.mdb,Test/resources/binary/FancyStuff.dll.mdb -resource:Test/resources/binary/SimpleWrite.dll,Test/resources/binary/SimpleWrite.dll -resource:Test/resources/binary/SimpleWrite.dll.mdb,Test/resources/binary/SimpleWrite.dll.mdb -resource:Test/resources/binary/Testing.dll,Test/resources/binary/Testing.dll -resource:Test/resources/binary/Testing.dll.mdb,Test/resources/binary/Testing.dll.mdb -resource:Test/resources/binary/XbuildReferenceBugTest.exe,Test/resources/binary/XbuildReferenceBugTest.exe -resource:Test/resources/binary/XbuildReferenceBugTest.exe.mdb,Test/resources/binary/XbuildReferenceBugTest.exe.mdb -resource:Test/resources/binary/XbuildReferenceBugTest2.exe,Test/resources/binary/XbuildReferenceBugTest2.exe
)

mono_declare_managed(
  NAME       Microsoft.Build.Tasks.dll
  PROFILES   unityjit
  OUTPUT_NAME Microsoft.Build.Tasks.v4.0.dll
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-unityjit
  REFS       System System.Core System.Xml System.Windows.Forms Microsoft.Build.Utilities.v4.0 Microsoft.Build.Framework Microsoft.Build.Engine Mono.XBuild.Tasks
  TEST_REFS  System.Xml Microsoft.Build.Engine Microsoft.Build.Framework Mono.XBuild.Tasks Microsoft.Build.Utilities.v4.0 System.Core Microsoft.Build
  TEST_FLAGS -resource:Test/resources/test.cs,Test/resources/test.cs -resource:Test/resources/test.dll,Test/resources/test.dll -resource:Test/resources/Sample.cs,Test/resources/Sample.cs -resource:Test/resources/Sample.vb,Test/resources/Sample.vb -resource:Test/resources/junk.txt,Test/resources/junk.txt -resource:Test/resources/binary/FancyStuff.dll,Test/resources/binary/FancyStuff.dll -resource:Test/resources/binary/FancyStuff.dll.mdb,Test/resources/binary/FancyStuff.dll.mdb -resource:Test/resources/binary/SimpleWrite.dll,Test/resources/binary/SimpleWrite.dll -resource:Test/resources/binary/SimpleWrite.dll.mdb,Test/resources/binary/SimpleWrite.dll.mdb -resource:Test/resources/binary/Testing.dll,Test/resources/binary/Testing.dll -resource:Test/resources/binary/Testing.dll.mdb,Test/resources/binary/Testing.dll.mdb -resource:Test/resources/binary/XbuildReferenceBugTest.exe,Test/resources/binary/XbuildReferenceBugTest.exe -resource:Test/resources/binary/XbuildReferenceBugTest.exe.mdb,Test/resources/binary/XbuildReferenceBugTest.exe.mdb -resource:Test/resources/binary/XbuildReferenceBugTest2.exe,Test/resources/binary/XbuildReferenceBugTest2.exe
)

mono_declare_managed(
  NAME       Microsoft.Build.Tasks.dll
  PROFILES   xbuild_12
  OUTPUT_NAME Microsoft.Build.Tasks.v12.0.dll
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/12.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_12
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml ../net_4_x/System.Windows.Forms Microsoft.Build.Utilities.v12.0 Microsoft.Build.Framework Microsoft.Build.Engine Mono.XBuild.Tasks
  TEST_REFS  ../net_4_x/System.Xml Microsoft.Build.Engine Microsoft.Build.Framework Mono.XBuild.Tasks Microsoft.Build.Utilities.v12.0 ../net_4_x/System.Core Microsoft.Build
  TEST_FLAGS -resource:Test/resources/test.cs,Test/resources/test.cs -resource:Test/resources/test.dll,Test/resources/test.dll -resource:Test/resources/Sample.cs,Test/resources/Sample.cs -resource:Test/resources/Sample.vb,Test/resources/Sample.vb -resource:Test/resources/junk.txt,Test/resources/junk.txt -resource:Test/resources/binary/FancyStuff.dll,Test/resources/binary/FancyStuff.dll -resource:Test/resources/binary/FancyStuff.dll.mdb,Test/resources/binary/FancyStuff.dll.mdb -resource:Test/resources/binary/SimpleWrite.dll,Test/resources/binary/SimpleWrite.dll -resource:Test/resources/binary/SimpleWrite.dll.mdb,Test/resources/binary/SimpleWrite.dll.mdb -resource:Test/resources/binary/Testing.dll,Test/resources/binary/Testing.dll -resource:Test/resources/binary/Testing.dll.mdb,Test/resources/binary/Testing.dll.mdb -resource:Test/resources/binary/XbuildReferenceBugTest.exe,Test/resources/binary/XbuildReferenceBugTest.exe -resource:Test/resources/binary/XbuildReferenceBugTest.exe.mdb,Test/resources/binary/XbuildReferenceBugTest.exe.mdb -resource:Test/resources/binary/XbuildReferenceBugTest2.exe,Test/resources/binary/XbuildReferenceBugTest2.exe
)

mono_declare_managed(
  NAME       Microsoft.Build.Tasks.dll
  PROFILES   xbuild_14
  OUTPUT_NAME Microsoft.Build.Tasks.Core.dll
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/14.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_14
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml ../net_4_x/System.Windows.Forms Microsoft.Build.Utilities.Core Microsoft.Build.Framework Microsoft.Build.Engine Mono.XBuild.Tasks
  TEST_REFS  ../net_4_x/System.Xml Microsoft.Build.Engine Microsoft.Build.Framework Mono.XBuild.Tasks Microsoft.Build.Utilities.Core ../net_4_x/System.Core Microsoft.Build
  TEST_FLAGS -resource:Test/resources/test.cs,Test/resources/test.cs -resource:Test/resources/test.dll,Test/resources/test.dll -resource:Test/resources/Sample.cs,Test/resources/Sample.cs -resource:Test/resources/Sample.vb,Test/resources/Sample.vb -resource:Test/resources/junk.txt,Test/resources/junk.txt -resource:Test/resources/binary/FancyStuff.dll,Test/resources/binary/FancyStuff.dll -resource:Test/resources/binary/FancyStuff.dll.mdb,Test/resources/binary/FancyStuff.dll.mdb -resource:Test/resources/binary/SimpleWrite.dll,Test/resources/binary/SimpleWrite.dll -resource:Test/resources/binary/SimpleWrite.dll.mdb,Test/resources/binary/SimpleWrite.dll.mdb -resource:Test/resources/binary/Testing.dll,Test/resources/binary/Testing.dll -resource:Test/resources/binary/Testing.dll.mdb,Test/resources/binary/Testing.dll.mdb -resource:Test/resources/binary/XbuildReferenceBugTest.exe,Test/resources/binary/XbuildReferenceBugTest.exe -resource:Test/resources/binary/XbuildReferenceBugTest.exe.mdb,Test/resources/binary/XbuildReferenceBugTest.exe.mdb -resource:Test/resources/binary/XbuildReferenceBugTest2.exe,Test/resources/binary/XbuildReferenceBugTest2.exe
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
