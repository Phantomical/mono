mono_declare_managed(
  NAME       System.Data.DataSetExtensions.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.Core System System.Data System.Xml
  FLAGS      -nowarn:436 -d:COREFX
  TEST_FLAGS -nowarn:436 -d:COREFX -nowarn:219 -nowarn:169 -resource:Test/System.Data/testdataset1.xml,Test/System.Data/testdataset1.xml
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
