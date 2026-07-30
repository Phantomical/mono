mono_declare_managed(
  NAME       System.Core.dll
  PROFILES   build net_4_x unityjit
  SNK        <topdir>/class/mono.snk
  REFS       System
  FLAGS      -d:FEATURE_PAL,PFX_LEGACY_3_5,FEATURE_NETCORE -d:INSIDE_SYSCORE -d:LIBC -unsafe -nowarn:436 -d:NET_3_5 -d:FEATURE_COMPILE,FEATURE_COMPILE_TO_METHODBUILDER
  TEST_FLAGS -d:FEATURE_PAL,PFX_LEGACY_3_5,FEATURE_NETCORE -d:INSIDE_SYSCORE -d:LIBC -unsafe -nowarn:436 -d:NET_3_5 -d:FEATURE_COMPILE,FEATURE_COMPILE_TO_METHODBUILDER
  XTEST_REFS System Facades/System.Threading.Tasks Microsoft.CSharp Facades/System.Text.RegularExpressions Facades/System.Reflection Facades/System.Reflection.Extensions Facades/System.Runtime.InteropServices.RuntimeInformation Facades/System.Reflection.Emit
  XTEST_FLAGS -unsafe -d:FEATURE_COMPILE
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
