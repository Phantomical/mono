# Converted from class/Facades/netstandard/Makefile.

mono_declare_managed(
  NAME       netstandard.dll
  PROFILES   build
  SUBDIR     Facades
  KEYFILE    ../../Open.snk
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System System.Xml System.Xml.Linq System.Core System.Numerics System.Net.Http System.IO.Compression System.ComponentModel.Composition System.IO.Compression.FileSystem System.Drawing
  FLAGS      /delaysign /nowarn:1616,1699,618 -d:SYSTEM_WEB_IMPLEMENTATION
  NO_DEBUG
)

mono_declare_managed(
  NAME       netstandard.dll
  PROFILES   net_4_x unityjit
  SUBDIR     Facades
  KEYFILE    ../../Open.snk
  INSTALL_DIR lib/mono/4.5/Facades
  REFS       System System.Xml System.Xml.Linq System.Core System.Numerics System.Net.Http System.IO.Compression System.ComponentModel.Composition System.IO.Compression.FileSystem System.Transactions System.Runtime.Serialization System.Data System.Data.DataSetExtensions System.Drawing
  FLAGS      /delaysign /nowarn:1616,1699,618 -d:SYSTEM_WEB_IMPLEMENTATION -unsafe
  NO_DEBUG
)
