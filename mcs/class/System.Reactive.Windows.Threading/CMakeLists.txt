mono_declare_managed(
  NAME       System.Reactive.Windows.Threading.dll
  PROFILES   net_4_x
  KEYFILE    ../reactive.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Reactive.Interfaces System.Reactive.Core System.Reactive.Linq WindowsBase
  FLAGS      -d:SIGNED -delaysign -resource:Strings_WindowsThreading.resources,System.Reactive.Strings_WindowsThreading.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  RESX       Strings_WindowsThreading.resources
  TEST_FLAGS -d:SIGNED -delaysign -resource:Strings_WindowsThreading.resources,System.Reactive.Strings_WindowsThreading.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  NO_TEST
)

mono_declare_managed(
  NAME       System.Reactive.Windows.Threading.dll
  PROFILES   unityjit
  KEYFILE    ../reactive.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Reactive.Interfaces System.Reactive.Core System.Reactive.Linq WindowsBase
  FLAGS      -d:SIGNED -delaysign -resource:Strings_WindowsThreading.resources,System.Reactive.Strings_WindowsThreading.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  RESX       Strings_WindowsThreading.resources
  TEST_FLAGS -d:SIGNED -delaysign -resource:Strings_WindowsThreading.resources,System.Reactive.Strings_WindowsThreading.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  NO_INSTALL
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
