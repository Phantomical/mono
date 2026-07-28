# The PE writer ilasm emits through.
#
# Only the bootstrap variant is declared here; the net_4_x one differs (it
# picks up Mono.Security and -d:HAS_MONO_SECURITY) and is added with that
# profile.
mono_declare_managed(
  NAME       PEAPI.dll
  PROFILES   build
  REFS       System
  FLAGS      -nowarn:414,618
  KEYFILE    ../mono.pub
  TARGET_NET_REFERENCE v4.7
  INTERMEDIATE)
