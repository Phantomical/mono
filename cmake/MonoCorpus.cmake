# Building and running managed test corpora.
#
# A corpus is a C#/IL program compiled by the class-library toolchain and run
# under the runtime this build produces.  Both mono/mini (the regression and
# tiering suites) and the per-pass checks under mono/unit-tests build them, so
# the compiler invocation, the fixture they hang off and the shape of a tiered
# test all live here rather than being spelled twice.
#
# Corpora are part of `all`, ordered after `mcs`: a finished build has every
# test input on disk, so running ctest never builds anything.

set(MONO_CORPUS_ENABLED FALSE)
if(NOT MONO_ENABLE_MCS_BUILD OR NOT MONO_ENABLE_EXECUTABLES)
  return()
endif()
set(MONO_CORPUS_ENABLED TRUE)

# The per-pass checks are Python; find the interpreter once, here, rather than
# in each directory that registers one.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MONO_CORPUS_CLASS_DIR "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
set(MONO_CORPUS_BUILD_DIR "${MONO_MCS_LIBDIR}/build")
set(MONO_CORPUS_WRAPPER   "${CMAKE_BINARY_DIR}/runtime/mono-wrapper")
set(MONO_CORPUS_CSFLAGS   -unsafe -nowarn:0219,0169,0414,0649,0618)

# TestDriver.dll is shared by every corpus but built with the mini ones, so a
# corpus living anywhere else has to be pointed at it: mono resolves a
# reference from the running assembly's own directory, which is no longer the
# same directory.
set(MONO_CORPUS_MINI_DIR "${CMAKE_BINARY_DIR}/mono/mini")
set(MONO_CORPUS_MONO_PATH "${MONO_CORPUS_CLASS_DIR}:${MONO_CORPUS_MINI_DIR}")

# csc and ilasm run on whichever mono MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS selected;
# the corpora themselves are always compiled -nostdlib against this tree's
# mscorlib, so the host makes no difference to what comes out.
mono_tools_runtime(_host MONO_PATH "${MONO_CORPUS_BUILD_DIR}")
set(MONO_CORPUS_CSC ${_host}
    "${MONO_CSC}" -langversion:8.0 -nostdlib -unsafe -nowarn:0162 -nologo -noconfig
    "-r:${MONO_CORPUS_CLASS_DIR}/mscorlib.dll"
    "-r:${MONO_CORPUS_CLASS_DIR}/System.dll"
    "-r:${MONO_CORPUS_CLASS_DIR}/System.Core.dll")
# -quiet drops the per-file banner and the success line; warnings and errors are
# printed either way.
set(MONO_CORPUS_ILASM ${_host} "${MONO_CORPUS_BUILD_DIR}/ilasm.exe" -quiet)

# Compile one C# corpus into the calling directory's binary dir, appending it to
# MONO_CORPUS_OUTPUTS in the caller's scope.
#
#   mono_corpus_cs(<out> [LIBRARY|MODULE] [DEBUG] SOURCES <src>... [REFS <dll>...])
#
# DEBUG emits a portable PDB next to the assembly, which is what lets the
# runtime turn an IL offset back into a source line.
#
# MODULE compiles to a netmodule, which mono_corpus_link () puts into an
# assembly.
function(mono_corpus_cs out)
  cmake_parse_arguments(ARG "LIBRARY;MODULE;DEBUG" "" "SOURCES;REFS" ${ARGN})
  set(_extra "")
  set(_outputs "${CMAKE_CURRENT_BINARY_DIR}/${out}")
  if(ARG_LIBRARY)
    set(_extra -target:library)
  endif()
  if(ARG_MODULE)
    set(_extra -target:module)
  endif()
  if(ARG_DEBUG)
    # mono derives the pdb name from the image by dropping a `.exe` or a `.dll`
    # and nothing else, so a netmodule wants its extension kept.
    cmake_path(GET out STEM _stem)
    set(_pdb "${CMAKE_CURRENT_BINARY_DIR}/${_stem}.pdb")
    if(ARG_MODULE)
      set(_pdb "${CMAKE_CURRENT_BINARY_DIR}/${out}.pdb")
    endif()
    list(APPEND _extra -debug:portable "-pdb:${_pdb}")
    list(APPEND _outputs "${_pdb}")
  endif()
  set(_srcs "")
  foreach(_s IN LISTS ARG_SOURCES)
    list(APPEND _srcs "${CMAKE_CURRENT_SOURCE_DIR}/${_s}")
  endforeach()
  # A reference built in this same directory has to be a dependency too, or a
  # parallel build compiles against a file that does not exist yet.  References
  # into the class libraries are covered by the mcs dependency below; references
  # built in another directory are covered by a target-level dependency, which
  # is the only kind that crosses directories reliably.
  set(_refs "")
  set(_ref_deps "")
  foreach(_r IN LISTS ARG_REFS)
    list(APPEND _refs "-r:${_r}")
    if(_r MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/")
      list(APPEND _ref_deps "${_r}")
    endif()
  endforeach()
  add_custom_command(
    OUTPUT ${_outputs}
    COMMAND ${MONO_CORPUS_CSC} ${_extra} ${MONO_CORPUS_CSFLAGS}
            "-out:${CMAKE_CURRENT_BINARY_DIR}/${out}" ${_srcs} ${_refs}
    DEPENDS ${_srcs} ${_ref_deps} mcs
    COMMENT "CSC ${out}"
    VERBATIM)
  set(MONO_CORPUS_OUTPUTS
      "${MONO_CORPUS_OUTPUTS};${_outputs}" PARENT_SCOPE)
endfunction()

# The same for an IL corpus.
#
#   mono_corpus_il(<out> <source.il> [LIBRARY|MODULE] [DEBUG])
#
# DEBUG writes an .mdb beside the assembly, which is what lets the runtime turn
# an IL offset back into a line and therefore what the transform needs before it
# will emit a sequence point.
#
# ilasm has no switch for a netmodule: what it writes is decided by the source,
# which gets an Assembly row only where it carries a `.assembly` directive.
# MODULE therefore says what the source already does, so that a caller naming a
# `.netmodule` and a source declaring an assembly is a configure-time error
# rather than a link that fails much later.
function(mono_corpus_il out source)
  cmake_parse_arguments(ARG "LIBRARY;MODULE;DEBUG" "" "" ${ARGN})
  set(_extra "")
  if(ARG_LIBRARY)
    set(_extra -dll)
  endif()
  if(ARG_MODULE)
    set(_extra -dll)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" _manifest
         REGEX "^[ \t]*\\.assembly[ \t]+[^e]")
    if(_manifest)
      message(FATAL_ERROR
        "${source} declares an assembly, so ilasm cannot make a module of it: ${_manifest}")
    endif()
  endif()
  if(ARG_DEBUG)
    list(APPEND _extra -debug)
  endif()
  get_filename_component(_outdir "${CMAKE_CURRENT_BINARY_DIR}/${out}" DIRECTORY)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_outdir}"
    COMMAND ${MONO_CORPUS_ILASM} ${_extra}
            "-output=${CMAKE_CURRENT_BINARY_DIR}/${out}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" mcs
    COMMENT "ILASM ${out}"
    VERBATIM)
  set(MONO_CORPUS_OUTPUTS
      "${MONO_CORPUS_OUTPUTS};${CMAKE_CURRENT_BINARY_DIR}/${out}" PARENT_SCOPE)
endfunction()

# Link netmodules into one assembly.
#
#   mono_corpus_link(<out> MODULES <mod>...)
#
# csc takes the assembly's name from the output file and its content from the
# modules, so no source is compiled here. The modules stay on disk beside the
# assembly and the runtime reads them from there, which is why they are outputs
# of this directory rather than of a temporary one.
#
# A type defined in a module is not in the assembly's own image. Anything keyed
# on the assembly name -- mono_class_is_magic_assembly () is the one that bites --
# does not see it, so a suite that needs its own name stays its own assembly.
function(mono_corpus_link out)
  cmake_parse_arguments(ARG "" "" "MODULES" ${ARGN})
  string(REPLACE ";" "," _addmodule "${ARG_MODULES}")
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
    COMMAND ${MONO_CORPUS_CSC} -target:library ${MONO_CORPUS_CSFLAGS}
            "-out:${CMAKE_CURRENT_BINARY_DIR}/${out}" "-addmodule:${_addmodule}"
    DEPENDS ${ARG_MODULES} mcs
    COMMENT "LINK ${out}"
    VERBATIM)
  set(MONO_CORPUS_OUTPUTS
      "${MONO_CORPUS_OUTPUTS};${CMAKE_CURRENT_BINARY_DIR}/${out}" PARENT_SCOPE)
endfunction()

# Attach this directory's corpora to `mini-corpora`, which is what carries them
# into `all`.  mono/mini owns that target and is processed first, so directories
# added later hang their own target off it.
#
#   mono_corpus_target(<name> [DEPENDS <target>...])
function(mono_corpus_target name)
  cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
  add_custom_target(${name} DEPENDS ${MONO_CORPUS_OUTPUTS})
  if(ARG_DEPENDS)
    add_dependencies(${name} ${ARG_DEPENDS})
  endif()
  add_dependencies(mini-corpora ${name})
endfunction()

# A check that runs a corpus and asserts on what the runtime recorded about it,
# rather than on what the corpus computed.  The script is Python under
# mono/unit-tests and takes the runtime, the corpus, and the corpus source
# carrying the expectations.
#
#   mono_add_corpus_check(<name> SCRIPT <py> CORPUS <exe> [SOURCE <cs>] [LABELS <l>...])
function(mono_add_corpus_check name)
  cmake_parse_arguments(ARG "" "SCRIPT;CORPUS;SOURCE" "LABELS" ${ARGN})
  # The check runs from the binary directory and reads the script out of the
  # source tree, which is no place to leave a __pycache__ behind.
  add_test(NAME ${name}
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${MONO_CORPUS_MONO_PATH}"
                   "PYTHONDONTWRITEBYTECODE=1"
                   "${Python3_EXECUTABLE}" "${ARG_SCRIPT}"
                   "${MONO_CORPUS_WRAPPER}" "${ARG_CORPUS}" ${ARG_SOURCE}
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  if(ARG_LABELS)
    set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}")
  endif()
endfunction()

