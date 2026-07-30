mono_declare_managed(
  NAME       System.Windows.Forms.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  SNK        <topdir>/class/mono.snk
  REFS       System System.Xml System.Drawing Accessibility System.Data Mono.Posix Mono.WebBrowser System.Configuration System.Runtime.Serialization.Formatters.Soap
  FLAGS      /unsafe @System.Windows.Forms.dll.resources -nowarn:618,612,809
  RESX       resources/keyboards.resources resources/System.Windows.Forms.resources
  RESOURCES  resources/NWSE.cur resources/NESW.cur resources/SplitterNS.cur resources/SplitterWE.cur resources/NWSE.cur resources/NESW.cur resources/DnDNo.cur resources/DnDCopy.cur resources/DnDLink.cur resources/DnDMove.cur
  TEST_REFS  System.Data System.Drawing Accessibility System System.Xml System.Runtime.Serialization.Formatters.Soap
  # The Interactive category opens windows and waits for a person.
  TEST_EXCLUDES Interactive
  TEST_FLAGS -resource:Test/resources/M.gif,Test/resources/M.gif -resource:Test/resources/a.cur,Test/resources/a.cur -resource:Test/resources/32x32.ico,Test/resources/32x32.ico -resource:Test/System.Resources/compat_1_1.resx,Test/System.Resources/compat_1_1.resx -resource:Test/System.Resources/compat_2_0.resx,Test/System.Resources/compat_2_0.resx -resource:Test/System.Windows.Forms/bitmaps/a.png,Test/System.Windows.Forms/bitmaps/a.png -nowarn:618,612
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
