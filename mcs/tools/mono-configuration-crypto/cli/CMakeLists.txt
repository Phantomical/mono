mono_declare_managed(
  PROGRAM
  NAME       mono-configuration-crypto.exe
  PROFILES   net_4_x
  INSTALL_DIR <libdir>/mono/mono-configuration-crypto/4.5
  REFS       Mono.Security System.Security System.Configuration System Mono.Configuration.Crypto
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
