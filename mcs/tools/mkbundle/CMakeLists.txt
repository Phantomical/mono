mono_declare_managed(
  PROGRAM
  NAME       mkbundle.exe
  PROFILES   net_4_x
  REFS       System.Xml System System.Core System.IO.Compression.FileSystem
  FLAGS      -resource:template.c -resource:template_z.c -resource:template_main.c -resource:template_common.inc -resource:bundle-mono-api.inc -d:STATIC,NO_SYMBOL_WRITER,NO_AUTHENTICODE
  RESOURCES  template.c template_z.c template_main.c template_common.inc bundle-mono-api.inc
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
