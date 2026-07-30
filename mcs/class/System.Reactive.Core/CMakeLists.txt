mono_declare_managed(
  NAME       System.Reactive.Core.dll
  PROFILES   net_4_x
  KEYFILE    ../reactive.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Reactive.Interfaces
  FLAGS      -d:SIGNED -delaysign -resource:Strings_Core.resources,System.Reactive.Strings_Core.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  RESX       Strings_Core.resources
  TEST_FLAGS -d:SIGNED -delaysign -resource:Strings_Core.resources,System.Reactive.Strings_Core.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  NO_TEST
)

mono_declare_managed(
  NAME       System.Reactive.Core.dll
  PROFILES   unityjit
  KEYFILE    ../reactive.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core System.Reactive.Interfaces
  FLAGS      -d:SIGNED -delaysign -resource:Strings_Core.resources,System.Reactive.Strings_Core.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  RESX       Strings_Core.resources
  TEST_FLAGS -d:SIGNED -delaysign -resource:Strings_Core.resources,System.Reactive.Strings_Core.resources -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  NO_INSTALL
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
