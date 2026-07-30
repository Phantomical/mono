mono_declare_managed(
  NAME       System.Reactive.PlatformServices.dll
  PROFILES   net_4_x
  KEYFILE    ../reactive.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Reactive.Interfaces System.Reactive.Core System.Reactive.Linq
  FLAGS      -d:SIGNED -delaysign -resource:Strings_PlatformServices.resources,System.Reactive.Strings_PlatformServices.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT -d:NO_TASK_DELAY
  RESX       Strings_PlatformServices.resources
  TEST_REFS  Mono.Reactive.Tests
  TEST_FLAGS -d:SIGNED -delaysign -resource:Strings_PlatformServices.resources,System.Reactive.Strings_PlatformServices.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT -d:NO_TASK_DELAY
  NO_TEST
)

mono_declare_managed(
  NAME       System.Reactive.PlatformServices.dll
  PROFILES   unityjit
  KEYFILE    ../reactive.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Reactive.Interfaces System.Reactive.Core System.Reactive.Linq
  FLAGS      -d:SIGNED -delaysign -resource:Strings_PlatformServices.resources,System.Reactive.Strings_PlatformServices.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT -d:NO_TASK_DELAY
  RESX       Strings_PlatformServices.resources
  TEST_REFS  Mono.Reactive.Tests
  TEST_FLAGS -d:SIGNED -delaysign -resource:Strings_PlatformServices.resources,System.Reactive.Strings_PlatformServices.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT -d:NO_TASK_DELAY
  NO_INSTALL
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
