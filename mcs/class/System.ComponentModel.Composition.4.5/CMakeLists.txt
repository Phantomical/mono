# The bootstrap copy has no embedded strings: resgen is a managed tool built in
# this same profile, and only the compiler needs this assembly there.
mono_declare_managed(
  NAME       System.ComponentModel.Composition.dll
  PROFILES   build
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core
  FLAGS      -d:CLR40 -d:USE_ECMA_KEY,FEATURE_REFLECTIONCONTEXT,FEATURE_REFLECTIONFILEIO,FEATURE_SERIALIZATION,FEATURE_SLIMLOCK -nowarn:219,414 -nowarn:436
)

mono_declare_managed(
  NAME       System.ComponentModel.Composition.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Core
  FLAGS      -d:CLR40 -d:USE_ECMA_KEY,FEATURE_REFLECTIONCONTEXT,FEATURE_REFLECTIONFILEIO,FEATURE_SERIALIZATION,FEATURE_SLIMLOCK -nowarn:219,414 -nowarn:436
  RESOURCE_DEFS Microsoft.Internal.Strings,src/ComponentModel/Strings.resx
  XTEST_REFS System System.Core System.Xml Facades/System.Threading.Tasks
  XTEST_FLAGS /keyfile:../mono.snk
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
