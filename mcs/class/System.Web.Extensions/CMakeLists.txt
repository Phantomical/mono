# Converted from class/System.Web.Extensions/Makefile.

mono_declare_managed(
  NAME       System.Web.Extensions.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../winfx.pub
  REFS       System System.Core System.Drawing System.Data System.Data.Linq System.Xml System.Web System.Web.Services System.Configuration System.EnterpriseServices System.ServiceModel System.Web.ApplicationServices
  FLAGS      -unsafe -define:NET_3_5 -nowarn:436 -define:SYSTEM_WEB_EXTENSIONS /resource:../MicrosoftAjaxLibrary/System.Web.Extensions/3.5.0.0/3.5.21022.8/MicrosoftAjax.debug.js /resource:../MicrosoftAjaxLibrary/System.Web.Extensions/3.5.0.0/3.5.21022.8/MicrosoftAjax.js /resource:../MicrosoftAjaxLibrary/System.Web.Extensions/3.5.0.0/3.5.21022.8/MicrosoftAjaxTimer.debug.js /resource:../MicrosoftAjaxLibrary/System.Web.Extensions/3.5.0.0/3.5.21022.8/MicrosoftAjaxTimer.js /resource:../MicrosoftAjaxLibrary/System.Web.Extensions/3.5.0.0/3.5.21022.8/MicrosoftAjaxWebForms.debug.js /resource:../MicrosoftAjaxLibrary/System.Web.Extensions/3.5.0.0/3.5.21022.8/MicrosoftAjaxWebForms.js
)
