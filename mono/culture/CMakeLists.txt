# Culture/locale tables.  Split out from mono/metadata purely because the
# generated tables are enormous and rebuild rarely.

mono_add_object_library(monoculture
  SOURCES
    w32process-unix-language.c
    locales.c
  INCLUDES "${CMAKE_SOURCE_DIR}/mono"
  DEPENDS mono::hidden mono::eglib_headers)
