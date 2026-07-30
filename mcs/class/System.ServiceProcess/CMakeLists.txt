mono_declare_managed(
  NAME       System.ServiceProcess.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Configuration.Install System.Windows.Forms
  FLAGS      /nowarn:0618
  TEST_REFS  System
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
