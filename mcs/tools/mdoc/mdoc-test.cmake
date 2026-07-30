# One case of mdoc's documentation-round-trip suite.
#
# Every case has the same shape: build one or more small assemblies, run mdoc
# over them into a directory, and diff that against a checked-in tree.  The
# Makefile ran them all through a single Test/en.actual in the source
# directory; here each case gets a private working copy of Test/ in the build
# tree, so the source stays clean and the cases run in parallel.
#
#   SETTINGS  generated file naming the runtime, mdoc.exe and the compiler
#   SRCDIR    this directory
#   WORKDIR   the case's private directory, which is also the working directory
#   CASE      which check to run

include("${SETTINGS}")

# ---------------------------------------------------------------------------
# The staging copy
# ---------------------------------------------------------------------------
# Copies, not symlinks: several of the compiles write an XML doc file back over
# an input name, which through a symlink would land in the source tree.  The
# expected trees are only ever read, so those are linked.
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/Test")

file(GLOB _inputs
     "${SRCDIR}/Test/*.cs" "${SRCDIR}/Test/*.xml" "${SRCDIR}/Test/*.dtd"
     "${SRCDIR}/Test/*.patch" "${SRCDIR}/Test/validate.check.*")
file(COPY ${_inputs} DESTINATION "${WORKDIR}/Test")

file(GLOB _expected LIST_DIRECTORIES true
     "${SRCDIR}/Test/en.expected*" "${SRCDIR}/Test/html.expected")
foreach(_e IN LISTS _expected)
  get_filename_component(_n "${_e}" NAME)
  file(CREATE_LINK "${_e}" "${WORKDIR}/Test/${_n}" SYMBOLIC)
endforeach()

# ---------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------
function(run)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORKDIR}"
                  RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    string(JOIN " " _what ${ARGN})
    message(FATAL_ERROR "${CASE}: failed (${_rc}): ${_what}")
  endif()
endfunction()

# One test assembly.  OUT and SOURCES are relative to WORKDIR, matching how the
# suite spells them on mdoc's own command lines.
#
# MDOC_CSC_FLAGS is the same flag set every other assembly in this tree is
# compiled with, and it has to be: index.xml records each assembly's attributes,
# and -optimize changes the DebuggableAttribute that lands there.
function(csc)
  cmake_parse_arguments(C "" "OUT;DOC;DEFINE" "SOURCES;REFS" ${ARGN})
  set(_extra "")
  if(C_DEFINE)
    list(APPEND _extra "/define:${C_DEFINE}")
  endif()
  if(C_DOC)
    list(APPEND _extra "-doc:${C_DOC}")
  endif()
  foreach(_r IN LISTS C_REFS)
    list(APPEND _extra "-r:${PROFILE_DIR}/${_r}.dll")
  endforeach()
  file(REMOVE "${WORKDIR}/${C_OUT}")
  run(${MDOC_CSC} ${MDOC_CSC_FLAGS} -nostdlib -unsafe -target:library
      "-r:${PROFILE_DIR}/mscorlib.dll" ${_extra} "-out:${C_OUT}" ${C_SOURCES})
endfunction()

function(mdoc)
  run("${CMAKE_COMMAND}" -E env "MONO_PATH=${PROFILE_DIR}"
      "${RUNTIME}" "${MDOC}" ${ARGN})
endfunction()

# Same as `diff -rup`: the trees have to match exactly, in both directions.
function(diff_tree expected actual)
  execute_process(COMMAND diff -rup "${expected}" "${actual}"
                  WORKING_DIRECTORY "${WORKDIR}"
                  OUTPUT_VARIABLE _d RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${CASE}: ${actual} differs from ${expected}\n${_d}")
  endif()
endfunction()

# DocTest.cs is the one source the suite edits in place: v1 is a copy, v2 is
# that copy with the checked-in patch applied.  The two versions are what the
# --since and --delete cases compare.
function(doctest version)
  cmake_parse_arguments(D "" "DOC" "" ${ARGN})
  file(COPY_FILE "${WORKDIR}/Test/DocTest-v1.cs" "${WORKDIR}/Test/DocTest.cs")
  if(version STREQUAL v2)
    execute_process(COMMAND patch -p0 -i DocTest-v2.patch
                    WORKING_DIRECTORY "${WORKDIR}/Test" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "${CASE}: applying DocTest-v2.patch failed (${_rc})")
    endif()
  endif()
  set(_doc "")
  if(D_DOC)
    set(_doc DOC "${D_DOC}")
  endif()
  csc(OUT Test/DocTest.dll SOURCES Test/DocTest.cs ${_doc}
      REFS System.Core Microsoft.CSharp)
endfunction()

set(_langs -lang docid -lang vb.net -lang fsharp -lang javascript
           -lang c++/cli -lang c++/cx -lang c++/winrt)

# ---------------------------------------------------------------------------
# The cases
# ---------------------------------------------------------------------------
if(CASE STREQUAL "monodocer")
  doctest(v1)
  # Run twice: the second pass over an existing tree has to be a no-op, which
  # is the property that keeps mdoc usable on a checked-in docset.
  mdoc(--debug update -o Test/en.actual Test/DocTest.dll ${_langs})
  diff_tree(Test/en.expected Test/en.actual)
  mdoc(--debug update -o Test/en.actual Test/DocTest.dll ${_langs})
  diff_tree(Test/en.expected Test/en.actual)

elseif(CASE STREQUAL "monodocer-since")
  doctest(v1)
  mdoc(--debug update -o Test/en.actual Test/DocTest.dll)
  doctest(v2)
  mdoc(--debug update "--since=Version 2.0" -o Test/en.actual Test/DocTest.dll)
  diff_tree(Test/en.expected.since Test/en.actual)

elseif(CASE STREQUAL "monodocer-delete")
  doctest(v1)
  mdoc(--debug update -o Test/en.actual Test/DocTest.dll)
  doctest(v2)
  mdoc(--debug update -o Test/en.actual Test/DocTest.dll)
  doctest(v1)
  mdoc(--debug update -fno-assembly-versions --delete
       -o Test/en.actual Test/DocTest.dll)
  diff_tree(Test/en.expected.delete Test/en.actual)

elseif(CASE STREQUAL "monodocer-importslashdoc")
  doctest(v1 DOC Test/DocTest.xml)
  mdoc(--debug update -i Test/DocTest.xml -o Test/en.actual Test/DocTest.dll)
  diff_tree(Test/en.expected.importslashdoc Test/en.actual)

elseif(CASE STREQUAL "monodocer-importecmadoc")
  doctest(v1)
  mdoc(--debug update -i Test/TestEcmaDocs.xml
       "--type=System.Action`1" --type=System.AsyncCallback
       --type=System.Environment --type=System.Array
       -o Test/en.actual Test/DocTest.dll)
  diff_tree(Test/en.expected.importecmadoc Test/en.actual)

elseif(CASE STREQUAL "monodocer-internal-interface")
  csc(OUT Test/DocTest-InternalInterface.dll
      SOURCES Test/DocTest-InternalInterface.cs)
  mdoc(update -o Test/en.actual Test/DocTest-InternalInterface.dll -lang VB.NET)
  diff_tree(Test/en.expected-internal-interface Test/en.actual)

elseif(CASE STREQUAL "monodocer-enumerations")
  csc(OUT Test/DocTest-enumerations.dll SOURCES Test/DocTest-enumerations.cs)
  mdoc(update -o Test/en.actual Test/DocTest-enumerations.dll)
  diff_tree(Test/en.expected-enumerations Test/en.actual)

elseif(CASE STREQUAL "monodocer-dropns-classic")
  csc(OUT Test/DocTest-DropNS-classic.dll SOURCES Test/DocTest-DropNS-classic.cs
      DOC Test/DocTest-DropNS-classic.xml)
  mdoc(update -o Test/en.actual Test/DocTest-DropNS-classic.dll --api-style=classic)
  csc(OUT Test/DocTest-DropNS-unified.dll SOURCES Test/DocTest-DropNS-unified.cs)
  mdoc(update --debug -o Test/en.actual Test/DocTest-DropNS-unified.dll
       --api-style=unified --dropns Test/DocTest-DropNS-unified.dll=MyFramework)
  diff_tree(Test/en.expected-dropns-classic-v1 Test/en.actual)

elseif(CASE STREQUAL "monodocer-dropns-classic-withsecondary")
  csc(OUT Test/DocTest-DropNS-classic.dll SOURCES Test/DocTest-DropNS-classic.cs
      DOC Test/DocTest-DropNS-classic.xml)
  csc(OUT Test/DocTest-DropNS-classic-secondary.dll
      SOURCES Test/DocTest-DropNS-classic-secondary.cs
      DOC Test/DocTest-DropNS-classic-secondary.xml)
  mdoc(update -o Test/en.actual Test/DocTest-DropNS-classic.dll
       Test/DocTest-DropNS-classic-secondary.dll --api-style=classic)
  csc(OUT Test/DocTest-DropNS-unified.dll SOURCES Test/DocTest-DropNS-unified.cs)
  mdoc(update -o Test/en.actual Test/DocTest-DropNS-unified.dll
       Test/DocTest-DropNS-classic-secondary.dll --api-style=unified
       --dropns Test/DocTest-DropNS-unified.dll=MyFramework)
  diff_tree(Test/en.expected-dropns-classic-withsecondary Test/en.actual)

elseif(CASE STREQUAL "monodocer-dropns-multi" OR
       CASE STREQUAL "monodocer-dropns-multi-withexisting")
  csc(OUT Test/DocTest-DropNS-classic.dll SOURCES Test/DocTest-DropNS-classic.cs
      DOC Test/DocTest-DropNS-classic.xml)
  csc(OUT Test/DocTest-DropNS-unified.dll SOURCES Test/DocTest-DropNS-unified.cs)
  csc(OUT Test/DocTest-DropNS-classic-multitest.dll
      SOURCES Test/DocTest-DropNS-classic.cs DEFINE MULTITEST)
  csc(OUT Test/DocTest-DropNS-unified-multitest.dll
      SOURCES Test/DocTest-DropNS-unified.cs DEFINE MULTITEST)

  set(_classic Test/DocTest-DropNS-classic.dll
               Test/DocTest-DropNS-classic-multitest.dll)
  set(_unified Test/DocTest-DropNS-unified.dll
               Test/DocTest-DropNS-unified-multitest.dll)
  set(_drop --dropns Test/DocTest-DropNS-unified.dll=MyFramework
            --dropns Test/DocTest-DropNS-unified-multitest.dll=MyFramework)

  if(CASE MATCHES "withexisting$")
    # Document one assembly of each style first, so the multi-assembly pass
    # below lands on a tree that already has entries in it.
    mdoc(update -o Test/en.actual Test/DocTest-DropNS-classic.dll --api-style=classic)
    mdoc(update -o Test/en.actual Test/DocTest-DropNS-unified.dll --api-style=unified
         --dropns Test/DocTest-DropNS-unified.dll=MyFramework)
  endif()

  mdoc(update -o Test/en.actual ${_classic} --api-style=classic)
  mdoc(update -o Test/en.actual ${_unified} --api-style=unified ${_drop})
  if(NOT CASE MATCHES "withexisting$")
    mdoc(update -o Test/en.actual ${_classic} --api-style=classic)
    mdoc(update -o Test/en.actual ${_unified} --api-style=unified ${_drop})
  endif()

  if(CASE MATCHES "withexisting$")
    diff_tree(Test/en.expected-dropns-multi-withexisting Test/en.actual)
  else()
    diff_tree(Test/en.expected-dropns-multi Test/en.actual)
  endif()

elseif(CASE STREQUAL "mdoc-export-html")
  mdoc(export-html -o Test/html.actual Test/en.expected.importslashdoc)
  diff_tree(Test/html.expected Test/html.actual)

elseif(CASE STREQUAL "mdoc-export-html-with-version")
  mdoc(export-html -o Test/html.actual.v0 Test/en.expected)
  mdoc(export-html -o Test/html.actual.since-with-v0 Test/en.expected.since
       -with-version 0.0.0.0)
  # The assertion is that -with-version adds no types: the two exports have to
  # contain the same set of files, whatever those files say.
  set(_lists "")
  foreach(_which v0 since-with-v0)
    file(GLOB_RECURSE _files RELATIVE "${WORKDIR}/Test/html.actual.${_which}"
         "${WORKDIR}/Test/html.actual.${_which}/*")
    list(SORT _files)
    string(JOIN "\n" _joined ${_files})
    list(APPEND _lists "${_joined}")
  endforeach()
  list(GET _lists 0 _list_v0)
  list(GET _lists 1 _list_since)
  if(NOT _list_v0 STREQUAL _list_since)
    message(FATAL_ERROR "${CASE}: -with-version changed the set of exported "
                        "files\n--- v0\n${_list_v0}\n--- since\n${_list_since}")
  endif()

elseif(CASE STREQUAL "mdoc-export-msxdoc")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${PROFILE_DIR}"
            "${RUNTIME}" "${MDOC}" export-msxdoc -o -
            Test/en.expected.importslashdoc
    WORKING_DIRECTORY "${WORKDIR}"
    OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${CASE}: mdoc export-msxdoc failed (${_rc})")
  endif()
  file(WRITE "${WORKDIR}/Test/msxdoc.actual.xml" "${_out}")
  diff_tree(Test/msxdoc-expected.importslashdoc.xml Test/msxdoc.actual.xml)

elseif(CASE STREQUAL "mdoc-validate")
  # validate reports the offending file as a file:// URL, so the expectations
  # were captured with the working directory stripped off the front.
  set(_trees en.expected en.expected.importslashdoc en.expected.since)
  foreach(_i RANGE 2)
    list(GET _trees ${_i} _tree)
    string(REPLACE "en.expected" "validate.check.monodocer" _expected "${_tree}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${PROFILE_DIR}"
              "${RUNTIME}" "${MDOC}" validate -f ecma "Test/${_tree}"
      WORKING_DIRECTORY "${WORKDIR}"
      OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REPLACE "file://${WORKDIR}/" "" _got "${_out}${_err}")
    file(READ "${WORKDIR}/Test/${_expected}" _want)
    if(NOT _got STREQUAL _want)
      message(FATAL_ERROR "${CASE}: validate ${_tree} differs from ${_expected}\n"
                          "--- expected\n${_want}--- actual\n${_got}")
    endif()
  endforeach()

else()
  message(FATAL_ERROR "unknown case ${CASE}")
endif()
