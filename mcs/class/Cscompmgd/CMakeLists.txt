mono_declare_managed(
  NAME       Cscompmgd.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME cscompmgd.dll
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System
  TEST_FLAGS -nowarn:0618 -nowarn:219 -nowarn:169
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
