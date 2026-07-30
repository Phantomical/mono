# Sets MCS_GENERATED_SOURCES and MCS_GENERATED_TARGETS.  Hand-written:
# it holds the jay or tool invocation the Makefile carried as a recipe.
include(${CMAKE_CURRENT_SOURCE_DIR}/generated-sources.cmake)

mono_declare_managed(
  NAME       Microsoft.Build.dll
  PROFILES   net_4_x
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-net_4_x
  REFS       System System.Core System.Xml Microsoft.Build.Engine Microsoft.Build.Framework
  FLAGS      /d:MICROSOFT_BUILD_DLL
  TEST_REFS  Microsoft.Build.Utilities.v4.0
  TEST_FLAGS /d:MICROSOFT_BUILD_DLL -resource:Test/resources/FunctionalTestReferenceProject.csproj,Test/resources/FunctionalTestReferenceProject.csproj -resource:Test/resources/FunctionalTestReferenceProject3.csproj,Test/resources/FunctionalTestReferenceProject3.csproj -resource:Test/resources/Microsoft.Build.Test.csproj,Test/resources/Microsoft.Build.Test.csproj -resource:Test/resources/Microsoft.Build.csproj,Test/resources/Microsoft.Build.csproj
  BUILT_SOURCES ${MCS_GENERATED_SOURCES}
  DEPENDS       ${MCS_GENERATED_TARGETS}
)

mono_declare_managed(
  NAME       Microsoft.Build.dll
  PROFILES   unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-unityjit
  REFS       System System.Core System.Xml Microsoft.Build.Engine Microsoft.Build.Framework
  FLAGS      /d:MICROSOFT_BUILD_DLL
  TEST_REFS  Microsoft.Build.Utilities.v4.0
  TEST_FLAGS /d:MICROSOFT_BUILD_DLL -resource:Test/resources/FunctionalTestReferenceProject.csproj,Test/resources/FunctionalTestReferenceProject.csproj -resource:Test/resources/FunctionalTestReferenceProject3.csproj,Test/resources/FunctionalTestReferenceProject3.csproj -resource:Test/resources/Microsoft.Build.Test.csproj,Test/resources/Microsoft.Build.Test.csproj -resource:Test/resources/Microsoft.Build.csproj,Test/resources/Microsoft.Build.csproj
  BUILT_SOURCES ${MCS_GENERATED_SOURCES}
  DEPENDS       ${MCS_GENERATED_TARGETS}
)

mono_declare_managed(
  NAME       Microsoft.Build.dll
  PROFILES   xbuild_12
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/12.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_12
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Engine Microsoft.Build.Framework
  FLAGS      /d:MICROSOFT_BUILD_DLL
  TEST_REFS  Microsoft.Build.Utilities.v12.0
  TEST_FLAGS /d:MICROSOFT_BUILD_DLL -resource:Test/resources/FunctionalTestReferenceProject.csproj,Test/resources/FunctionalTestReferenceProject.csproj -resource:Test/resources/FunctionalTestReferenceProject3.csproj,Test/resources/FunctionalTestReferenceProject3.csproj -resource:Test/resources/Microsoft.Build.Test.csproj,Test/resources/Microsoft.Build.Test.csproj -resource:Test/resources/Microsoft.Build.csproj,Test/resources/Microsoft.Build.csproj
  BUILT_SOURCES ${MCS_GENERATED_SOURCES}
  DEPENDS       ${MCS_GENERATED_TARGETS}
)

mono_declare_managed(
  NAME       Microsoft.Build.dll
  PROFILES   xbuild_14
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/14.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_14
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Engine Microsoft.Build.Framework
  FLAGS      /d:MICROSOFT_BUILD_DLL
  TEST_REFS  Microsoft.Build.Utilities.Core
  TEST_FLAGS /d:MICROSOFT_BUILD_DLL -resource:Test/resources/FunctionalTestReferenceProject.csproj,Test/resources/FunctionalTestReferenceProject.csproj -resource:Test/resources/FunctionalTestReferenceProject3.csproj,Test/resources/FunctionalTestReferenceProject3.csproj -resource:Test/resources/Microsoft.Build.Test.csproj,Test/resources/Microsoft.Build.Test.csproj -resource:Test/resources/Microsoft.Build.csproj,Test/resources/Microsoft.Build.csproj
  BUILT_SOURCES ${MCS_GENERATED_SOURCES}
  DEPENDS       ${MCS_GENERATED_TARGETS}
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
