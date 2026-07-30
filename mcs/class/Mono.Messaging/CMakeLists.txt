mono_declare_managed(
  NAME       Mono.Messaging.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Configuration
  TEST_REFS  System.Messaging
  TEST_FLAGS -nowarn:0618 -nowarn:219 -nowarn:169
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
