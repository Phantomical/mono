mono_declare_managed(
  NAME       System.Security.Cryptography.Pkcs.dll
  PROFILES   unityjit
  SUBDIR     Facades
  KEYFILE    ../../msfinal.pub
  SNK        <topdir>/class/mono.snk
  INSTALL_DIR <libdir>/mono/4.5/Facades
  REFS       System System.Security
  FLAGS      /delaysign /nowarn:1616,1699
  NO_DEBUG
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
