mono_declare_managed(
  NAME       System.Threading.Tasks.Dataflow.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System
  FLAGS      -d:CONCURRENT_COLLECTIONS
  TEST_REFS  System.Core System
  XTEST_REFS System System.Core System.Xml Facades/System.Threading.Tasks
  XTEST_REMOTE_EXECUTOR
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
