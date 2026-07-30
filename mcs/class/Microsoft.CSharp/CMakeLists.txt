mono_declare_managed(
  NAME       Microsoft.CSharp.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System
  XTEST_REFS Microsoft.CSharp System.Core System Facades/System.Threading.Tasks Facades/System.Runtime.InteropServices.RuntimeInformation
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
