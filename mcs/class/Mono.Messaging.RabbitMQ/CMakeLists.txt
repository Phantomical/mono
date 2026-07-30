mono_declare_managed(
  NAME       Mono.Messaging.RabbitMQ.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Messaging Mono.Messaging RabbitMQ.Client
  FLAGS      -nowarn:618
  TEST_FLAGS -nowarn:618 -nowarn:0618 -nowarn:219 -nowarn:169
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
