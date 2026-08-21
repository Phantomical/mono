# The monodoc documentation sources: three assembled trees (the embedding API,
# the tools' man pages, the file-format descriptions) plus the .source files
# monodoc reads to find them.
#
# Off by default. It needs mdoc.exe out of the class libraries and perl for
# exdoc, and it is the one part of the tree whose output no other part of the
# build consumes, so paying for it on every build would be wrong. Turn it on
# with -DMONO_ENABLE_DOCS=ON.

if(NOT MONO_ENABLE_DOCS)
  return()
endif()

if(NOT MONO_ENABLE_MCS_BUILD OR NOT MONO_ENABLE_EXECUTABLES)
  message(FATAL_ERROR
    "MONO_ENABLE_DOCS needs the class libraries: the documentation is "
    "assembled by mdoc.exe running on the runtime this build produces")
endif()

if(NOT MONO_PERL)
  message(FATAL_ERROR
    "MONO_ENABLE_DOCS needs perl: exdoc turns the in-source API comments into "
    "the monodoc XML the trees are assembled from")
endif()

set(_src      "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin      "${CMAKE_CURRENT_BINARY_DIR}")
set(_class    "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
set(_buildcls "${MONO_MCS_LIBDIR}/build")
set(_wrapper  "${CMAKE_BINARY_DIR}/runtime/mono-wrapper")

mono_tools_runtime(_host MONO_PATH "${_buildcls}" AOT_PATH "${_buildcls}")
set(_csc ${_host} "${MONO_CSC}" -debug:portable -noconfig -nologo)

# mdoc runs on the built runtime against the built class libraries: it is one
# of them, so only that runtime can load it.
set(_mdoc ${CMAKE_COMMAND} -E env "MONO_PATH=${_class}"
          "${_wrapper}" "${_class}/mdoc.exe")

# The HTML converter used to turn the in-source API comments into monodoc XML
file(GLOB _agility_sources CONFIGURE_DEPENDS "${_src}/HtmlAgilityPack/*.cs")

add_custom_command(
  OUTPUT "${_bin}/AgilityPack.dll"
  COMMAND ${_csc} -target:library "-out:${_bin}/AgilityPack.dll"
          "-r:${_class}/mscorlib.dll"
          "-r:${_class}/System.dll"
          "-r:${_class}/System.Xml.dll"
          ${_agility_sources}
  DEPENDS ${_agility_sources} mcs
  COMMENT "CSC AgilityPack.dll"
  VERBATIM)

add_custom_command(
  OUTPUT "${_bin}/convert.exe"
  COMMAND ${_csc} "-out:${_bin}/convert.exe"
          "-r:${_class}/mscorlib.dll"
          "-r:${_class}/System.Xml.dll"
          "-r:${_bin}/AgilityPack.dll"
          "${_src}/convert.cs"
  DEPENDS "${_src}/convert.cs" "${_bin}/AgilityPack.dll" mcs
  COMMENT "CSC convert.exe"
  VERBATIM)

# exdoc: extracts the API documentation out of the runtime's C sources
#
# The comment blocks live in mono/*/*.c, so the stamp has to be rebuilt when any
# of them changes. Globbing that at configure time would be a lie the moment a
# file is added, but it is the same bargain the old build made and the target is
# opt-in.
file(GLOB _runtime_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/mono/*/*.c")

add_custom_command(
  OUTPUT "${_bin}/deploy/.stamp"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/html"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/deploy"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_src}/api-style.css" "${_bin}/deploy/"
  # exdoc shells out to "$ENV{runtimedir}/mono-wrapper convert.exe ...", so it
  # needs runtimedir set and needs to run where convert.exe was built.
  COMMAND ${CMAKE_COMMAND} -E env "MONO_PATH=${_class}"
          "runtimedir=${CMAKE_BINARY_DIR}/runtime"
          "${MONO_PERL}" "${_src}/exdoc" -h "${_src}" -t "${_bin}"
          ${_runtime_sources}
  COMMAND ${CMAKE_COMMAND} -E touch "${_bin}/deploy/.stamp"
  DEPENDS "${_bin}/convert.exe" "${_src}/exdoc" ${_runtime_sources}
  WORKING_DIRECTORY "${_bin}"
  COMMENT "Extracting embedded API documentation"
  VERBATIM)
add_custom_target(docs-extract DEPENDS "${_bin}/deploy/.stamp")

# The assembled trees
#
# `mdoc assemble` writes both the .tree and the .zip, so they are one command
# with two outputs rather than the two rules with a recovery test that the make
# version needed.
#
# The working directory matters, and it is not the same for all three. Every
# input holds paths relative to the directory mdoc runs in: mono-tools.config
# and mono-file-formats.config point at `../man/*.1` in the source tree, while
# toc.xml points at `deploy/*.html`, which exdoc wrote into the build tree.
# Building in-tree gave the old rules both at once. Out of tree, they have to
# be told apart. The output path is absolute either way, so nothing lands in
# the source directory.
function(_mono_assemble_doc name format workdir input)
  add_custom_command(
    OUTPUT "${_bin}/${name}.tree" "${_bin}/${name}.zip"
    COMMAND ${_mdoc} assemble -o "${_bin}/${name}" -f ${format} "${input}"
    DEPENDS "${input}" ${ARGN}
    WORKING_DIRECTORY "${workdir}"
    COMMENT "MDOC assemble ${name}"
    VERBATIM)
endfunction()

# monoapi is the extracted embedding API, so it waits on exdoc. The other two
# are assembled straight from checked-in config files.
_mono_assemble_doc(monoapi           hb  "${_bin}" "${_src}/toc.xml"
                   "${_bin}/deploy/.stamp")
_mono_assemble_doc(mono-tools        man "${_src}" "${_src}/mono-tools.config")
_mono_assemble_doc(mono-file-formats man "${_src}" "${_src}/mono-file-formats.config")

add_custom_target(docs ALL
  DEPENDS "${_bin}/monoapi.tree"           "${_bin}/monoapi.zip"
          "${_bin}/mono-tools.tree"        "${_bin}/mono-tools.zip"
          "${_bin}/mono-file-formats.tree" "${_bin}/mono-file-formats.zip")

install(FILES
    "${_src}/mono-file-formats.source"
    "${_src}/mono-tools.source"
    "${_src}/monoapi.source"
    "${_bin}/mono-file-formats.tree" "${_bin}/mono-file-formats.zip"
    "${_bin}/mono-tools.tree"        "${_bin}/mono-tools.zip"
    "${_bin}/monoapi.tree"           "${_bin}/monoapi.zip"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/monodoc/sources")

# Doxygen runs over the runtime sources. It is unrelated to monodoc, and
# useful only if you have doxygen. It is never part of `all`.
if(MONO_DOXYGEN)
  add_custom_target(doxygen
    COMMAND "${MONO_DOXYGEN}" "${_src}/doxyfile"
    WORKING_DIRECTORY "${_src}"
    COMMENT "Running doxygen"
    VERBATIM)
endif()
