mono_declare_managed(
  NAME       System.Net.Http.dll
  PROFILES   build net_4_x
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System
  FLAGS      -unsafe -nowarn:436
  TEST_REFS  System System.Core
)

mono_declare_managed(
  NAME       System.Net.Http.dll
  PROFILES   unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System Mono.Security
  FLAGS      -unsafe -nowarn:436 -d:SECURITY_DEP
  TEST_REFS  System System.Core
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
