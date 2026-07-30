mono_declare_managed(
  NAME       System.Runtime.Serialization.Formatters.Soap.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Xml
  TEST_REFS  System
  TEST_FLAGS -nowarn:0618 -nowarn:219 -nowarn:169
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
