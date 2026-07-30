mono_declare_managed(
  NAME       netstandard.dll
  PROFILES   build
  SUBDIR     Facades
  KEYFILE    ../../Open.snk
  SNK        ../../Open.snk
  INSTALL_DIR <libdir>/mono/4.5/Facades
  REFS       System System.Xml System.Xml.Linq System.Core System.Numerics System.Net.Http System.IO.Compression System.ComponentModel.Composition System.IO.Compression.FileSystem System.Drawing
  FLAGS      /delaysign /nowarn:1616,1699,618 -d:SYSTEM_WEB_IMPLEMENTATION
  NO_DEBUG
  NO_TEST
)

mono_declare_managed(
  NAME       netstandard.dll
  PROFILES   net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../Open.snk
  SNK        ../../Open.snk
  INSTALL_DIR <libdir>/mono/4.5/Facades
  REFS       System System.Xml System.Xml.Linq System.Core System.Numerics System.Net.Http System.IO.Compression System.ComponentModel.Composition System.IO.Compression.FileSystem System.Web System.Transactions System.Runtime.Serialization System.Data System.Data.DataSetExtensions System.Drawing
  FLAGS      /delaysign /nowarn:1616,1699,618
  NO_DEBUG
  NO_TEST
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
