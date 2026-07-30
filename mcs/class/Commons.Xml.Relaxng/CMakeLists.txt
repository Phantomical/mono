# Sets MCS_GENERATED_SOURCES and MCS_GENERATED_TARGETS.  Hand-written:
# it holds the jay or tool invocation the Makefile carried as a recipe.
include(${CMAKE_CURRENT_SOURCE_DIR}/generated-sources.cmake)

mono_declare_managed(
  NAME       Commons.Xml.Relaxng.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml
  FLAGS      /resource:resources/relaxng.rng,relaxng.rng
  RESOURCES  resources/relaxng.rng
  TEST_FLAGS /resource:resources/relaxng.rng,relaxng.rng -nowarn:0618 -nowarn:219 -nowarn:169 -resource:Test/XmlFiles/463264.rng,Test/XmlFiles/463264.rng -resource:Test/XmlFiles/463267.rng,Test/XmlFiles/463267.rng -resource:Test/XmlFiles/SimpleElementPattern1.rng,Test/XmlFiles/SimpleElementPattern1.rng -resource:Test/XmlFiles/SimpleElementPattern2.rng,Test/XmlFiles/SimpleElementPattern2.rng -resource:Test/XmlFiles/nvdl.rng,Test/XmlFiles/nvdl.rng -resource:Test/XmlFiles/relaxng.rng,Test/XmlFiles/relaxng.rng -resource:Test/XmlFiles/team.rng,Test/XmlFiles/team.rng -resource:Test/XmlFiles/atom.rnc,Test/XmlFiles/atom.rnc -resource:Test/XmlFiles/include-default-namespace-included.rnc,Test/XmlFiles/include-default-namespace-included.rnc -resource:Test/XmlFiles/include-default-namespace.rnc,Test/XmlFiles/include-default-namespace.rnc -resource:Test/XmlFiles/relaxng.rnc,Test/XmlFiles/relaxng.rnc -resource:Test/XmlFiles/schemas-xmlsoap-or-ws-2005-05-identity.rnc,Test/XmlFiles/schemas-xmlsoap-or-ws-2005-05-identity.rnc -resource:Test/XmlFiles/nvdl.nvdl,Test/XmlFiles/nvdl.nvdl -resource:Test/XmlFiles/SimpleElementPattern1.xml,Test/XmlFiles/SimpleElementPattern1.xml -resource:Test/XmlFiles/SimpleElementPattern2.xml,Test/XmlFiles/SimpleElementPattern2.xml -resource:Test/XmlFiles/include-default-namespace.xml,Test/XmlFiles/include-default-namespace.xml -resource:Test/XmlFiles/team.xml,Test/XmlFiles/team.xml
  BUILT_SOURCES ${MCS_GENERATED_SOURCES}
  DEPENDS       ${MCS_GENERATED_TARGETS}
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
