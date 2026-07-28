# Converted from class/System.ServiceProcess/Makefile.

mono_declare_managed(
  NAME       System.ServiceProcess.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System System.Configuration.Install System.Windows.Forms
  FLAGS      /nowarn:0618
)
