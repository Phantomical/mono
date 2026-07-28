# Converted from class/System.Messaging/Makefile.

mono_declare_managed(
  NAME       System.Messaging.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System System.Xml Mono.Messaging System.Configuration.Install System.Drawing System.Windows.Forms
  FLAGS      /resource:System.Messaging/MessageQueue.resx
)
