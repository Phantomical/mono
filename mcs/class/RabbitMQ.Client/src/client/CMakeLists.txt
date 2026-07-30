# Sets MCS_GENERATED_SOURCES and MCS_GENERATED_TARGETS.  Hand-written:
# it holds the jay or tool invocation the Makefile carried as a recipe.
include(${CMAKE_CURRENT_SOURCE_DIR}/generated-sources.cmake)

mono_declare_managed(
  NAME       RabbitMQ.Client.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../../../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml
  FLAGS      -nowarn:618
  BUILT_SOURCES ${MCS_GENERATED_SOURCES}
  DEPENDS       ${MCS_GENERATED_TARGETS}
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
