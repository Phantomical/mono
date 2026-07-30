# The PE writer ilasm emits through.
#
# The bootstrap variant compiles against the checked-in reference assemblies
# and lands in tmp/, because ilasm has to run on system mono before this tree
# has a BCL.  Everywhere else it picks up Mono.Security for strong-name
# signing.
mono_declare_managed(
  NAME       PEAPI.dll
  PROFILES   build
  REFS       System
  FLAGS      -nowarn:414,618
  KEYFILE    ../mono.pub
  TARGET_NET_REFERENCE v4.7
  INTERMEDIATE)

mono_declare_managed(
  NAME       PEAPI.dll
  PROFILES   net_4_x unityjit
  REFS       System Mono.Security
  FLAGS      -nowarn:414,618 -d:HAS_MONO_SECURITY
  KEYFILE    ../mono.pub)
