mono_declare_managed(
  PROGRAM
  NAME       mdoc.exe
  PROFILES   net_4_x
  REFS       monodoc System System.Xml System.Core Mono.Cecil ICSharpCode.SharpZipLib System.Xml.Linq System.Web
  FLAGS      /resource:../../../external/api-doc-tools/mdoc/Resources/defaulttemplate.xsl,defaulttemplate.xsl /resource:../../../external/api-doc-tools/mdoc/Resources/monodoc-ecma.xsd,monodoc-ecma.xsd /resource:../../../external/api-doc-tools/mdoc/Resources/msitomsx.xsl,msitomsx.xsl /resource:../../../external/api-doc-tools/mdoc/Resources/overview.xsl,overview.xsl /resource:../../../external/api-doc-tools/mdoc/Resources/stylesheet.xsl,stylesheet.xsl /resource:../../../external/api-doc-tools/monodoc/Resources/mdoc-html-format.xsl,mdoc-html-format.xsl /resource:../../../external/api-doc-tools/monodoc/Resources/mdoc-html-utils.xsl,mdoc-html-utils.xsl /resource:../../../external/api-doc-tools/monodoc/Resources/mdoc-sections-css.xsl,mdoc-sections-css.xsl /resource:../../../external/api-doc-tools/monodoc/Resources/mono-ecma-css.xsl,mono-ecma-css.xsl -define:NEW_CECIL
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
