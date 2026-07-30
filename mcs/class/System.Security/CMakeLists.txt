mono_declare_managed(
  NAME       System.Security.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       Mono.Security System System.Xml
  API_BIN_REFS System.Numerics System.Core
  FLAGS      -unsafe -nowarn:414,618 -d:SECURITY_DEP
  TEST_FLAGS -unsafe -nowarn:414,618 -d:SECURITY_DEP -nowarn:168,169,183,219,414 -resource:Test/System.Security.Cryptography.Xml/sample.pfx,Test/System.Security.Cryptography.Xml/sample.pfx -resource:Test/System.Security.Cryptography.Xml/EncryptedXmlSample1.xml,Test/System.Security.Cryptography.Xml/EncryptedXmlSample1.xml -resource:Test/System.Security.Cryptography.Xml/EncryptedXmlSample2.xml,Test/System.Security.Cryptography.Xml/EncryptedXmlSample2.xml -resource:Test/System.Security.Cryptography.Xml/EncryptedXmlSample3.xml,Test/System.Security.Cryptography.Xml/EncryptedXmlSample3.xml -resource:Test/System.Security.Cryptography.Pkcs/detached.data,Test/System.Security.Cryptography.Pkcs/detached.data -resource:Test/System.Security.Cryptography.Pkcs/detached.p7,Test/System.Security.Cryptography.Pkcs/detached.p7
  XTEST_REFS System.Core System Facades/System.Linq Facades/System.Threading.Tasks
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
