# Culture/locale tables.  Split out from mono/metadata purely because the
# generated tables are enormous and rebuild rarely.

# w32process-unix-language.c answers VerLanguageName for a host that has no
# such call.  Windows has one, and mono/metadata/w32process-win32.c forwards to
# it, so compiling both defines the entry point twice.
if(MONO_HOST_WINDOWS)
  set(monoculture_platform_sources "")
else()
  set(monoculture_platform_sources w32process-unix-language.c)
endif()

mono_add_object_library(monoculture
  SOURCES
    ${monoculture_platform_sources}
    locales.c
  INCLUDES "${CMAKE_SOURCE_DIR}/mono"
  DEPENDS mono::hidden mono::eglib_headers)
