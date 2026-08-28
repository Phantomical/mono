# The generator behind mono/culture/culture-info-tables.h.
#
# Off by default, and deliberately not part of `all`: running it downloads a
# CLDR release over the network and rewrites a checked-in generated header. You
# want it when you are changing locale coverage, and never otherwise.

if(NOT MONO_ENABLE_LOCALE_BUILDER OR NOT MONO_ENABLE_MCS_BUILD
   OR NOT MONO_ENABLE_EXECUTABLES)
  return()
endif()

set(_src      "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin      "${CMAKE_CURRENT_BINARY_DIR}")
set(_class    "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
set(_buildcls "${MONO_MCS_LIBDIR}/build")
set(_wrapper  "${MONO_RUNTIME_WRAPPER}")

mono_tools_runtime(_host MONO_PATH "${_buildcls}" AOT_PATH "${_buildcls}")

# -noconfig drops csc.rsp, so the references the tool needs have to be named
# explicitly, out of the profile the tool will run against.
set(_refs
  "-r:${_class}/System.dll"
  "-r:${_class}/System.Xml.dll"
  "-r:${_class}/System.Core.dll")

set(_sources
  Driver.cs CalendarType.cs Constants.cs Patterns.cs
  CultureInfoEntry.cs DateTimeFormatEntry.cs NumberFormatEntry.cs
  RegionInfoEntry.cs TextInfoEntry.cs Entry.cs)

set(_abs_sources "")
foreach(_s IN LISTS _sources)
  list(APPEND _abs_sources "${_src}/${_s}")
endforeach()

add_custom_command(
  OUTPUT "${_bin}/locale-builder.exe"
  COMMAND ${_host} "${MONO_CSC}" -debug:portable -noconfig -nologo
          ${_refs} "-out:${_bin}/locale-builder.exe" ${_abs_sources}
  DEPENDS ${_abs_sources} mcs
  COMMENT "CSC locale-builder.exe"
  VERBATIM)
add_custom_target(locale-builder DEPENDS "${_bin}/locale-builder.exe")

# The CLDR data the generator reads. Fetched into the build tree, so a clean
# build directory refetches and the source tree stays untouched.
set(MONO_CLDR_VERSION "30.0.2" CACHE STRING
    "CLDR release the culture tables are generated from")

add_custom_command(
  OUTPUT "${_bin}/CLDR/common/supplemental/supplementalData.xml"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/CLDR"
  COMMAND ${CMAKE_COMMAND} -E echo
          "Downloading CLDR ${MONO_CLDR_VERSION}"
  COMMAND ${CMAKE_COMMAND} -E chdir "${_bin}"
          ${CMAKE_COMMAND} -DURL=http://www.unicode.org/Public/cldr/${MONO_CLDR_VERSION}/core.zip
                           -DDEST=${_bin}/core.zip
                           -DEXTRACT_TO=${_bin}/CLDR
                           -P "${CMAKE_SOURCE_DIR}/cmake/MonoFetchCldr.cmake"
  COMMENT "Fetching CLDR ${MONO_CLDR_VERSION}"
  VERBATIM)
add_custom_target(locale-data
  DEPENDS "${_bin}/CLDR/common/supplemental/supplementalData.xml")

# The generator resolves CLDR/ relative to its working directory.
set(MONO_MINIMAL_LOCALES "en" CACHE STRING
    "Regex over locales/ selecting what `culture-table-minimal` keeps")

add_custom_target(culture-table
  COMMAND "${_wrapper}" "${_bin}/locale-builder.exe"
  WORKING_DIRECTORY "${_bin}"
  COMMENT "Generating culture-info-tables.h (all locales)"
  USES_TERMINAL
  VERBATIM)
add_dependencies(culture-table locale-builder locale-data)

add_custom_target(culture-table-minimal
  COMMAND "${_wrapper}" "${_bin}/locale-builder.exe"
          --locales "${MONO_MINIMAL_LOCALES}"
  WORKING_DIRECTORY "${_bin}"
  COMMENT "Generating culture-info-tables.h (${MONO_MINIMAL_LOCALES})"
  USES_TERMINAL
  VERBATIM)
add_dependencies(culture-table-minimal locale-builder locale-data)

# Copies the result back over the checked-in header, which is what makes the
# regeneration take effect. Separate from generating it so you can diff first.
add_custom_target(install-culture-table
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_bin}/culture-info-tables.h"
          "${CMAKE_SOURCE_DIR}/mono/culture/culture-info-tables.h"
  COMMENT "Updating mono/culture/culture-info-tables.h"
  VERBATIM)
