mono_declare_managed(
  NAME       Mono.Configuration.Crypto.dll
  PROFILES   net_4_x
  SNK        <topdir>/class/mono.snk
  INSTALL_DIR <libdir>/mono/mono-configuration-crypto/4.5
  REFS       System Mono.Security System.Security System.Configuration System.Xml
  NO_SIGN
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
