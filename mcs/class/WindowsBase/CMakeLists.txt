mono_declare_managed(
  NAME       WindowsBase.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx3.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Xaml
  FLAGS      -unsafe -nowarn:67,618
  TEST_REFS  WindowsBase System System.Xml System.Core System.IO.Compression System.Xaml
  TEST_FLAGS -unsafe
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
