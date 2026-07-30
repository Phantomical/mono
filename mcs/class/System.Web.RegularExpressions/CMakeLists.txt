mono_declare_managed(
  NAME       System.Web.RegularExpressions.dll
  PROFILES   net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  FLAGS      -delaysign -keyfile:../msfinal.pub
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
