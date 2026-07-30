mono_declare_managed(
  NAME       System.Reactive.Interfaces.dll
  PROFILES   net_4_x
  KEYFILE    ../reactive.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core
  FLAGS      -d:SIGNED -delaysign -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  TEST_FLAGS -d:SIGNED -delaysign -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  NO_TEST
)

mono_declare_managed(
  NAME       System.Reactive.Interfaces.dll
  PROFILES   unityjit
  KEYFILE    ../reactive.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core
  FLAGS      -d:SIGNED -delaysign -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  TEST_FLAGS -d:SIGNED -delaysign -d:HAS_EDI -d:PREFERASYNC -d:PREFER_ASYNC -d:HAS_AWAIT
  NO_INSTALL
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
