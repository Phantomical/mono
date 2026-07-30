mono_declare_managed(
  NAME       System.Net.Http.WebRequest.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Net.Http System System.Core
  TEST_REFS  System System.Core System.Net.Http System.Net.Http.WebRequest
  TEST_FLAGS -d:WEBREQUEST_HANDLER
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
