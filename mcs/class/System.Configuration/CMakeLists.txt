mono_declare_managed(
  NAME       System.Configuration.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_GLOBAL Test/test-config-file
  # The <appSettings file="..."/> in that fragment names this.
  TEST_RUNNER_FILES Test/appSettings.config,System.Configuration-appSettings.config
  REFS       System.Security System System.Xml
  FLAGS      -nowarn:618
  TEST_REFS  System.Xml System
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
