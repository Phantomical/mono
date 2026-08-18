# gettext catalogues for the mcs compiler's diagnostics.
#
# Worth knowing before you spend time here: nothing reads these. The compiler
# has no gettext binding -- no Catalog.GetString, no bindtextdomain, no
# Mono.Unix reference anywhere in mcs/mcs -- so the translated strings never
# reach a user. The automake build compiled and installed them anyway, and this
# does the same rather than quietly dropping four languages' worth of
# translation work; wiring the compiler up to them is a separate job.

if(NOT MONO_ENABLE_NLS)
  return()
endif()

if(NOT MONO_MSGFMT)
  message(STATUS "msgfmt not found; the gettext catalogues will not be built")
  return()
endif()

set(_domain "mcs")
set(_podir  "${CMAKE_CURRENT_SOURCE_DIR}/mcs")

# LINGUAS is the list gettext itself uses, so it stays the one place a language
# is added.
file(READ "${_podir}/LINGUAS" _linguas_raw)
string(REGEX REPLACE "#[^\n]*" "" _linguas_raw "${_linguas_raw}")
string(REGEX REPLACE "[ \t\r\n]+" ";" _linguas "${_linguas_raw}")
list(REMOVE_ITEM _linguas "")

set(_catalogues "")
foreach(_lang IN LISTS _linguas)
  set(_mo "${CMAKE_CURRENT_BINARY_DIR}/${_lang}.mo")
  add_custom_command(
    OUTPUT "${_mo}"
    COMMAND "${MONO_MSGFMT}" -c -o "${_mo}" "${_podir}/${_lang}.po"
    DEPENDS "${_podir}/${_lang}.po"
    COMMENT "MSGFMT ${_lang}.mo"
    VERBATIM)
  list(APPEND _catalogues "${_mo}")
  install(FILES "${_mo}"
          DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${_lang}/LC_MESSAGES"
          RENAME "${_domain}.mo")
endforeach()

add_custom_target(mcs-catalogues ALL DEPENDS ${_catalogues})

# Regenerating the template. Maintainer-only, and off `all` because it rewrites
# a checked-in file. The keyword list is the one from po/mcs/Makevars: mcs
# reports diagnostics through Report.Error/Report.Warning rather than _().
if(MONO_XGETTEXT)
  add_custom_target(update-mcs-pot
    COMMAND "${MONO_XGETTEXT}"
            --default-domain=${_domain}
            --directory="${CMAKE_SOURCE_DIR}"
            --files-from="${_podir}/POTFILES.in"
            --keyword=_ --keyword=N_
            "--keyword=Report.Error:3" "--keyword=Report.Error:2"
            "--keyword=Report.Warning:3" "--keyword=Report.Warning:2"
            "--language=C#" --from-code=UTF-8
            "--copyright-holder=Novell, Inc."
            "--msgid-bugs-address=http://www.mono-project.com/Bugs"
            --output="${_podir}/${_domain}.pot"
    COMMENT "XGETTEXT ${_domain}.pot"
    VERBATIM)
endif()
