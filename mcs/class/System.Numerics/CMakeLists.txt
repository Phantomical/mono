mono_declare_managed(
  NAME       System.Numerics.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System
  FLAGS      /unsafe
  TEST_FLAGS /unsafe
  XTEST_REFS System Facades/System.Threading.Tasks Facades/System.Runtime.InteropServices.RuntimeInformation System.Core Microsoft.CSharp
  XTEST_FLAGS -unsafe
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
