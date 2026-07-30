mono_declare_managed(
  NAME       System.Web.Services.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  SNK        <topdir>/class/mono.snk
  REFS       System.EnterpriseServices System.Xml System.Data System.Design System.DirectoryServices System.Configuration
  API_BIN_REFS System System.Web
  FLAGS      -nowarn:168,612,618,649 -d:MONO_BROKEN_CONFIGURATION_DLL -resource:System.Web.Services.Description/wsdl-1.1.xsd,wsdl-1.1.xsd -resource:System.Web.Services.Description/wsdl-1.1-soap.xsd,wsdl-1.1-soap.xsd -resource:System.Web.Services.Description/web-reference.xsd,web-reference.xsd
  TEST_FLAGS -nowarn:168,612,618,649 -d:MONO_BROKEN_CONFIGURATION_DLL -resource:System.Web.Services.Description/wsdl-1.1.xsd,wsdl-1.1.xsd -resource:System.Web.Services.Description/wsdl-1.1-soap.xsd,wsdl-1.1-soap.xsd -resource:System.Web.Services.Description/web-reference.xsd,web-reference.xsd -nowarn:618 -d:MONO_FEATURE_THREAD_ABORT -d:MONO_FEATURE_THREAD_SUSPEND_RESUME -resource:Test/System.Web.Services.Description/443095.wsdl,Test/System.Web.Services.Description/443095.wsdl -resource:Test/System.Web.Services.Description/6041.wsdl,Test/System.Web.Services.Description/6041.wsdl -resource:Test/System.Web.Services.Description/check-import.wsdl,Test/System.Web.Services.Description/check-import.wsdl -resource:Test/System.Web.Services.Description/check-import2.wsdl,Test/System.Web.Services.Description/check-import2.wsdl -resource:Test/System.Web.Services.Description/test.wsdl,Test/System.Web.Services.Description/test.wsdl -resource:Test/System.Web.Services.Description/test2.wsdl,Test/System.Web.Services.Description/test2.wsdl -resource:Test/System.Web.Services.Description/test3.wsdl,Test/System.Web.Services.Description/test3.wsdl
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
