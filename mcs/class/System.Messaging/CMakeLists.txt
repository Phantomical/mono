mono_declare_managed(
  NAME       System.Messaging.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml Mono.Messaging System.Configuration.Install System.Drawing System.Windows.Forms
  FLAGS      /resource:System.Messaging/MessageQueue.resx
  TEST_FLAGS /resource:System.Messaging/MessageQueue.resx -nowarn:0618 -nowarn:219 -nowarn:169
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
