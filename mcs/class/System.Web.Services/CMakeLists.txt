# Converted from class/System.Web.Services/Makefile.

mono_declare_managed(
  NAME       System.Web.Services.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System.EnterpriseServices System.Xml System.Data System.Design System.DirectoryServices System.Configuration
  API_BIN_REFS System System.Web
  FLAGS      -nowarn:168,612,618,649 -d:MONO_BROKEN_CONFIGURATION_DLL -resource:System.Web.Services.Description/wsdl-1.1.xsd,wsdl-1.1.xsd -resource:System.Web.Services.Description/wsdl-1.1-soap.xsd,wsdl-1.1-soap.xsd -resource:System.Web.Services.Description/web-reference.xsd,web-reference.xsd
)
