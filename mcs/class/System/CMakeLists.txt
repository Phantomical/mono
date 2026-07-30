mono_declare_managed(
  NAME       System.dll
  PROFILES   build net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_GLOBAL Test/test-config-file
  REFS       MonoSecurity=Mono.Security
  API_BIN_REFS System.Net.Http System.Xml System.Core System.Numerics System.Configuration
  FLAGS      -d:COREFX -d:CONFIGURATION_2_0 -d:SYSTEM_NET_PRIMITIVES_DLL -d:XML_DEP -d:SECURITY_DEP -d:FEATURE_PAL,SYSTEM_NAMESPACE,MONO,PLATFORM_UNIX -d:MONO_FEATURE_PROCESS_START -d:MONO_FEATURE_THREAD_ABORT -d:MONO_FEATURE_THREAD_SUSPEND_RESUME -d:MONO_FEATURE_MULTIPLE_APPDOMAINS -unsafe -resource:resources/Asterisk.wav -resource:resources/Beep.wav -resource:resources/Exclamation.wav -resource:resources/Hand.wav -resource:resources/Question.wav -nowarn:436 -d:MONO_SECURITY_ALIAS -d:CODEDOM -d:FEATURE_COMPILED -d:MONO_FEATURE_BTLS -d:ENABLE_GSS -d:CONFIGURATION_DEP
  TEST_REFS  Mono.Security System.Data System.Xml System.Core System.Configuration System.Drawing
  TEST_FLAGS -nowarn:618,672,219,67,169,612 -resource:Test/System/test-uri-props.txt,Test/System/test-uri-props.txt -resource:Test/System/test-uri-props-manual.txt,Test/System/test-uri-props-manual.txt -resource:Test/System/test-uri-relative-props.txt,Test/System/test-uri-relative-props.txt -resource:Test/compressed.bin,Test/compressed.bin -d:MONO_FEATURE_PROCESS_START -d:MONO_FEATURE_THREAD_ABORT -d:MONO_FEATURE_THREAD_SUSPEND_RESUME -d:MONO_FEATURE_MULTIPLE_APPDOMAINS
  XTEST_REFS System System.Core Facades/System.Threading.Tasks Facades/System.Runtime.InteropServices.RuntimeInformation System.Net.Http
  XTEST_FLAGS -resource:Test/TestData/testclienteku.contoso.com.pfx -resource:Test/TestData/testnoeku.contoso.com.pfx -resource:Test/TestData/testselfsignedservereku.contoso.com.pfx -resource:Test/TestData/testservereku.contoso.com.pfx
  XTEST_REMOTE_EXECUTOR
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
