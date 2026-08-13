# Tests that need more than "compile this one source against TestDriver".
#
# MONO_TESTS_SPECIAL names every .exe built here so the bulk loop in
# CMakeLists.txt skips it.

set(MONO_TESTS_SPECIAL "")

# Helper: build the assembly and record it as handled.
macro(_mono_special out)
  mono_test_assembly("${out}" ${ARGN})
  if("${out}" MATCHES "\\.exe$")
    list(APPEND MONO_TESTS_SPECIAL "${out}")
  endif()
endmacro()

# --- libraries the tests reference ------------------------------------------
_mono_special(TestingReferenceAssembly.dll LIBRARY NO_DEFAULT_REFS
              SOURCES TestingReferenceAssembly.cs)
_mono_special(TestingReferenceReferenceAssembly.dll LIBRARY NO_DEFAULT_REFS
              SOURCES TestingReferenceReferenceAssembly.cs
              REFS TestingReferenceAssembly.dll)
_mono_special(reference-loader.exe SOURCES reference-loader.cs
              REFS TestingReferenceAssembly.dll TestingReferenceReferenceAssembly.dll)

# Types that do not exist in the desktop profile, so tests can still use them.
_mono_special(Mono.Runtime.Testing.dll LIBRARY NO_DEFAULT_REFS SOURCES weakattribute.cs)
_mono_special(weak-fields.exe SOURCES weak-fields.cs REFS Mono.Runtime.Testing.dll)

_mono_special(test-inline-call-stack-library.dll LIBRARY NO_DEFAULT_REFS
              SOURCES test-inline-call-stack-library.cs)
_mono_special(test-inline-call-stack.exe SOURCES test-inline-call-stack.cs
              REFS test-inline-call-stack-library.dll)

_mono_special(null-blob-tgt.dll LIBRARY NO_DEFAULT_REFS SOURCES null-blob-tgt.cs)
_mono_special(null-blob-ref.dll LIBRARY IL SOURCES null-blob-ref.il)
_mono_special(null-blob-null-blob-assm.dll LIBRARY IL SOURCES null-blob-null-blob-assm.il)
_mono_special(null-blob-main.exe SOURCES null-blob-main.cs
              REFS null-blob-tgt.dll null-blob-ref.dll null-blob-null-blob-assm.dll)

_mono_special(load-missing.dll LIBRARY IL SOURCES load-missing.il)

# --- generic boxing/unboxing, custom modifiers -------------------------------
_mono_special(generic-unboxing.2.dll LIBRARY IL SOURCES generic-unboxing.2.il)
_mono_special(generic-boxing.2.dll   LIBRARY IL SOURCES generic-boxing.2.il
              DEPENDS "${_bin}/generic-unboxing.2.dll")
_mono_special(generic-unbox.2.exe SOURCES generic-unbox.2.cs NO_DEFAULT_REFS
              REFS generic-unboxing.2.dll)
_mono_special(generic-box.2.exe SOURCES generic-box.2.cs NO_DEFAULT_REFS
              REFS generic-unboxing.2.dll generic-boxing.2.dll)
_mono_special(generic-delegate2-lib.2.dll LIBRARY IL SOURCES generic-delegate2-lib.2.il)
_mono_special(generic-delegate2.2.exe SOURCES generic-delegate2.2.cs NO_DEFAULT_REFS
              REFS generic-delegate2-lib.2.dll)

_mono_special(custom-modifiers-lib.dll LIBRARY IL SOURCES custom-modifiers-lib.il)
_mono_special(custom-modifiers.2.exe SOURCES custom-modifiers.2.cs NO_DEFAULT_REFS
              REFS custom-modifiers-lib.dll)

_mono_special(bug-324535-il.dll LIBRARY IL SOURCES bug-324535-il.il)
_mono_special(bug-324535.exe SOURCES bug-324535.cs NO_DEFAULT_REFS REFS bug-324535-il.dll)

_mono_special(bug-81466-lib.dll LIBRARY IL SOURCES bug-81466-lib.il)
_mono_special(bug-81466.exe IL SOURCES bug-81466.il DEPENDS "${_bin}/bug-81466-lib.dll")

_mono_special(bug-382986-lib.dll LIBRARY NO_DEFAULT_REFS SOURCES bug-382986-lib.cs)
_mono_special(bug-382986.exe SOURCES bug-382986.cs NO_DEFAULT_REFS REFS bug-382986-lib.dll)

# bug-3903 checks that a v2.0-targeted assembly still loads, so it is compiled
# against the reference assemblies rather than the built class libraries.
_mono_special(bug-3903.exe NO_DEFAULT_REFS SOURCES bug-3903.cs
              FLAGS -nostdlib
              REFS "${CMAKE_SOURCE_DIR}/external/binary-reference-assemblies/v2.0/mscorlib.dll"
                   "${CMAKE_SOURCE_DIR}/external/binary-reference-assemblies/v2.0/System.Core.dll")

_mono_special(bug-80307.exe SOURCES bug-80307.cs NO_DEFAULT_REFS
              REFS "${_class}/System.Web.dll")

# The corpus is compiled unoptimized, and the shape this one is about - a
# value-type call result flowing into a loop-carried local across a try - only
# survives in optimized IL.
_mono_special(vtype-return-in-try.exe SOURCES vtype-return-in-try.cs
              FLAGS -optimize)

# --- tests whose library is rebuilt after the test, to make the test see a
# --- different definition at run time than it compiled against --------------
# bug-81673 and bug-36848 both link a library, then overwrite it with a
# WITH_STOP build so the loaded assembly no longer matches.
foreach(_pair "bug-81673:bug-81673-interface" "bug-36848:bug-36848-a")
  string(REPLACE ":" ";" _p "${_pair}")
  list(GET _p 0 _test)
  list(GET _p 1 _lib)
  add_custom_command(
    OUTPUT "${_bin}/${_test}.exe"
    COMMAND ${_csc_unsafe} -target:library "-out:${_bin}/${_lib}.dll" "${_src}/${_lib}.cs"
    COMMAND ${_csc_unsafe} "-r:${_bin}/${_lib}.dll" "-out:${_bin}/${_test}.exe" "${_src}/${_test}.cs"
    COMMAND ${_csc_unsafe} -d:WITH_STOP -target:library "-out:${_bin}/${_lib}.dll" "${_src}/${_lib}.cs"
    DEPENDS "${_src}/${_test}.cs" "${_src}/${_lib}.cs" mono-test-toolchain
    WORKING_DIRECTORY "${_bin}"
    COMMENT "CSC ${_test}.exe"
    VERBATIM)
  list(APPEND _all_assemblies "${_bin}/${_test}.exe")
  list(APPEND MONO_TESTS_SPECIAL "${_test}.exe")
endforeach()

# bug-81691 deletes the intermediate library so the test has to resolve it the
# hard way.
add_custom_command(
  OUTPUT "${_bin}/bug-81691.exe"
  COMMAND ${_csc_unsafe} -target:library "-out:${_bin}/bug-81691-a.dll" "${_src}/bug-81691-a.cs"
  COMMAND ${_csc_unsafe} "-r:${_bin}/bug-81691-a.dll" -target:library
          "-out:${_bin}/bug-81691-b.dll" "${_src}/bug-81691-b.cs"
  COMMAND ${_csc_unsafe} "-r:${_bin}/bug-81691-b.dll" "-out:${_bin}/bug-81691.exe" "${_src}/bug-81691.cs"
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${_bin}/bug-81691-a.dll"
  DEPENDS "${_src}/bug-81691.cs" "${_src}/bug-81691-a.cs" "${_src}/bug-81691-b.cs"
          mono-test-toolchain
  WORKING_DIRECTORY "${_bin}"
  COMMENT "CSC bug-81691.exe"
  VERBATIM)
list(APPEND _all_assemblies "${_bin}/bug-81691.exe")
list(APPEND MONO_TESTS_SPECIAL bug-81691.exe)

# bug-17537 checks that a non-executable assembly still loads.
add_custom_command(
  OUTPUT "${_bin}/bug-17537-helper.exe"
  COMMAND ${_csc_unsafe} "-out:${_bin}/bug-17537-helper.exe" "${_src}/bug-17537-helper.cs"
  COMMAND chmod -x "${_bin}/bug-17537-helper.exe"
  DEPENDS "${_src}/bug-17537-helper.cs" mono-test-toolchain
  COMMENT "CSC bug-17537-helper.exe"
  VERBATIM)
_mono_special(bug-17537.exe SOURCES bug-17537.cs DEPENDS "${_bin}/bug-17537-helper.exe")

# --- type-load and reflection suites ----------------------------------------
# load-exceptions wants t.dll to be missing the type it compiled against.
add_custom_command(
  OUTPUT "${_bin}/load-exceptions.exe"
  COMMAND ${_csc_unsafe} -t:library "-out:${_bin}/t.dll" -d:FOUND "${_src}/t-missing.cs"
  COMMAND ${_csc_unsafe} "-r:${_bin}/TestDriver.dll" "-r:${_bin}/load-missing.dll"
          "-r:${_bin}/t.dll" "-out:${_bin}/load-exceptions.exe" "${_src}/load-exceptions.cs"
  COMMAND ${_csc_unsafe} -t:library "-out:${_bin}/t.dll" "${_src}/t-missing.cs"
  DEPENDS "${_src}/load-exceptions.cs" "${_src}/t-missing.cs" "${_bin}/load-missing.dll"
          mono-test-toolchain
  WORKING_DIRECTORY "${_bin}"
  COMMENT "CSC load-exceptions.exe"
  VERBATIM)
list(APPEND _all_assemblies "${_bin}/load-exceptions.exe")
list(APPEND MONO_TESTS_SPECIAL load-exceptions.exe)

add_custom_command(
  OUTPUT "${_bin}/custom-attr-errors.exe"
  COMMAND ${_csc_unsafe} -t:library "-out:${_bin}/custom-attr-errors-lib.dll"
          -d:WITH_MEMBERS "${_src}/custom-attr-errors-lib.cs"
  COMMAND ${_csc_unsafe} "-r:${_bin}/TestDriver.dll" "-r:${_bin}/custom-attr-errors-lib.dll"
          "-out:${_bin}/custom-attr-errors.exe" "${_src}/custom-attr-errors.cs"
  COMMAND ${_csc_unsafe} -t:library "-out:${_bin}/custom-attr-errors-lib.dll"
          "${_src}/custom-attr-errors-lib.cs"
  DEPENDS "${_src}/custom-attr-errors.cs" "${_src}/custom-attr-errors-lib.cs"
          mono-test-toolchain
  WORKING_DIRECTORY "${_bin}"
  COMMENT "CSC custom-attr-errors.exe"
  VERBATIM)
list(APPEND _all_assemblies "${_bin}/custom-attr-errors.exe")
list(APPEND MONO_TESTS_SPECIAL custom-attr-errors.exe)

_mono_special(reflection-load-with-context-second-lib.dll LIBRARY NO_DEFAULT_REFS
              SOURCES reflection-load-with-context-second-lib.cs)
_mono_special(reflection-load-with-context-lib.dll LIBRARY NO_DEFAULT_REFS
              SOURCES reflection-load-with-context-lib.cs
              REFS reflection-load-with-context-second-lib.dll)
_mono_special(reflection-load-with-context.exe SOURCES reflection-load-with-context.cs
              DEPENDS "${_bin}/reflection-load-with-context-lib.dll")

# --- netmodules --------------------------------------------------------------
_mono_special(test-multi-netmodule-1-netmodule.netmodule MODULE NO_DEFAULT_REFS
              SOURCES test-multi-netmodule-1-netmodule.cs)
foreach(_n 2-dll1 3-dll2)
  add_custom_command(
    OUTPUT "${_bin}/test-multi-netmodule-${_n}.dll"
    COMMAND ${_csc_unsafe} -target:library
            "-addmodule:${_bin}/test-multi-netmodule-1-netmodule.netmodule"
            "-out:${_bin}/test-multi-netmodule-${_n}.dll"
            "${_src}/test-multi-netmodule-${_n}.cs"
    DEPENDS "${_src}/test-multi-netmodule-${_n}.cs"
            "${_bin}/test-multi-netmodule-1-netmodule.netmodule" mono-test-toolchain
    WORKING_DIRECTORY "${_bin}"
    COMMENT "CSC test-multi-netmodule-${_n}.dll"
    VERBATIM)
endforeach()
_mono_special(test-multi-netmodule-4-exe.exe NO_DEFAULT_REFS
              SOURCES test-multi-netmodule-4-exe.cs
              REFS test-multi-netmodule-2-dll1.dll test-multi-netmodule-3-dll2.dll)

_mono_special(modules-m1.netmodule MODULE NO_DEFAULT_REFS SOURCES modules-m1.cs)
add_custom_command(
  OUTPUT "${_bin}/modules.exe"
  COMMAND ${_csc_unsafe} "-addmodule:${_bin}/modules-m1.netmodule"
          "-r:${_bin}/TestDriver.dll" "-out:${_bin}/modules.exe" "${_src}/modules.cs"
  DEPENDS "${_src}/modules.cs" "${_bin}/modules-m1.netmodule" mono-test-toolchain
  WORKING_DIRECTORY "${_bin}"
  COMMENT "CSC modules.exe"
  VERBATIM)
list(APPEND _all_assemblies "${_bin}/modules.exe")
list(APPEND MONO_TESTS_SPECIAL modules.exe)

# --- assembly loading --------------------------------------------------------
# Two directories holding same-named assemblies, which is the whole point.
foreach(_dir assembly-load-dir1 assembly-load-dir2)
  _mono_special(${_dir}/Lib.dll LIBRARY NO_DEFAULT_REFS SOURCES ${_dir}/Lib.cs)
  add_custom_command(
    OUTPUT "${_bin}/${_dir}/LibStrongName.dll"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bin}/${_dir}"
    COMMAND ${_csc_unsafe} -target:library "-out:${_bin}/${_dir}/LibStrongName.dll"
            "${_src}/${_dir}/LibStrongName.cs" "-keyfile:${_src}/testing_gac/testkey.snk"
    DEPENDS "${_src}/${_dir}/LibStrongName.cs" "${_src}/testing_gac/testkey.snk"
            mono-test-toolchain
    COMMENT "CSC ${_dir}/LibStrongName.dll"
    VERBATIM)
  list(APPEND _all_assemblies "${_bin}/${_dir}/LibStrongName.dll")
endforeach()

# The output name here is deliberately all lower case: the test is about
# case-insensitive simple-name resolution.
_mono_special(assembly-load-dir1/LibSimpleName.dll LIBRARY NO_DEFAULT_REFS
              SOURCES assembly-load-dir1/LibSimpleName.cs)
_mono_special(assembly-load-dir2/libsimplename.dll LIBRARY NO_DEFAULT_REFS
              SOURCES assembly-load-dir2/LibSimpleName.cs)
_mono_special(assembly-dep-simplename.dll LIBRARY NO_DEFAULT_REFS
              SOURCES assembly-dep-simplename.cs
              REFS assembly-load-dir1/LibSimpleName.dll)

set(_lib_dep "${_bin}/assembly-load-dir1/Lib.dll" "${_bin}/assembly-load-dir2/Lib.dll")
set(_sn_dep  "${_bin}/assembly-load-dir1/LibStrongName.dll"
             "${_bin}/assembly-load-dir2/LibStrongName.dll")
_mono_special(assembly-load-bytes.exe SOURCES assembly-load-bytes.cs DEPENDS ${_lib_dep})
_mono_special(assembly-loadfrom.exe   SOURCES assembly-loadfrom.cs   DEPENDS ${_lib_dep})
_mono_special(assembly-loadfile.exe   SOURCES assembly-loadfile.cs   DEPENDS ${_lib_dep})
_mono_special(assembly-loadfrom-bindingredirect.exe
              SOURCES assembly-loadfrom-bindingredirect.cs DEPENDS ${_sn_dep})
_mono_special(assembly-loadfile-bindingredirect.exe
              SOURCES assembly-loadfile-bindingredirect.cs DEPENDS ${_sn_dep})
_mono_special(assembly-load-bytes-bindingredirect.exe
              SOURCES assembly-load-bytes-bindingredirect.cs DEPENDS ${_sn_dep})
_mono_special(assembly-refonly-load-bytes-bindingredirect.exe
              SOURCES assembly-refonly-load-bytes-bindingredirect.cs DEPENDS ${_sn_dep})
_mono_special(assembly-loadfrom-simplename.exe SOURCES assembly-loadfrom-simplename.cs
              DEPENDS "${_bin}/assembly-dep-simplename.dll"
                      "${_bin}/assembly-load-dir2/libsimplename.dll")

# assemblyresolve_* exercise the AssemblyResolve event, so the dependencies
# deliberately live in a directory that is not on the assembly search path.
_mono_special(assemblyresolve_deps/TestBase.dll LIBRARY NO_DEFAULT_REFS
              SOURCES assemblyresolve_TestBase.cs)
_mono_special(assemblyresolve_deps/Test.dll LIBRARY NO_DEFAULT_REFS
              SOURCES assemblyresolve_Test.cs
              REFS assemblyresolve_deps/TestBase.dll)
# TestBase has to be named explicitly: csc will not follow the transitive
# reference out of a directory that is not on the assembly search path, which
# is exactly the situation this test sets up.
_mono_special(assemblyresolve_asm.dll LIBRARY NO_DEFAULT_REFS
              SOURCES assemblyresolve_asm.cs
              REFS assemblyresolve_deps/TestBase.dll assemblyresolve_deps/Test.dll)
_mono_special(assemblyresolve_deps/assemblyresolve_event5_label.dll LIBRARY NO_DEFAULT_REFS
              SOURCES assemblyresolve_event5_label.cs)
_mono_special(assemblyresolve_event5_helper.dll LIBRARY NO_DEFAULT_REFS
              SOURCES assemblyresolve_event5_helper.cs
              REFS assemblyresolve_deps/assemblyresolve_event5_label.dll)
foreach(_n 3 4 6)
  _mono_special(assemblyresolve_event${_n}.exe SOURCES assemblyresolve_event${_n}.cs
                DEPENDS "${_bin}/assemblyresolve_asm.dll"
                        "${_bin}/assemblyresolve_deps/Test.dll"
                        "${_bin}/assemblyresolve_deps/TestBase.dll")
endforeach()
_mono_special(assemblyresolve_event5.exe SOURCES assemblyresolve_event5.cs
              DEPENDS "${_bin}/assemblyresolve_event5_helper.dll")

# appdomain-marshalbyref-assemblyload loads the same leaf assembly from two
# directories, one of them built with a method left out.
_mono_special(LeafAssembly.dll LIBRARY NO_DEFAULT_REFS
              SOURCES appdomain-marshalbyref-assemblyload-LeafAssembly.cs)
_mono_special(appdomain-marshalbyref-assemblyload2/LeafAssembly.dll LIBRARY NO_DEFAULT_REFS
              SOURCES appdomain-marshalbyref-assemblyload-LeafAssembly.cs
              DEFINES UNDEFINE_OTHER_METHOD)
_mono_special(MidAssembly.dll LIBRARY NO_DEFAULT_REFS
              SOURCES appdomain-marshalbyref-assemblyload-MidAssembly.cs
              REFS LeafAssembly.dll)
_mono_special(appdomain-marshalbyref-assemblyload.exe NO_DEFAULT_REFS
              SOURCES appdomain-marshalbyref-assemblyload.cs
              REFS MidAssembly.dll LeafAssembly.dll
              DEPENDS "${_bin}/appdomain-marshalbyref-assemblyload2/LeafAssembly.dll")

# --- tests that launch another test as a child process ----------------------
# The three programs below are only ever named as prerequisites, so automake
# built them through its implicit %.exe rules and they never appear in a test
# list. They still have to be built.
_mono_special(appdomain-tester.exe SOURCES appdomain-tester.cs)
_mono_special(event-il.exe IL SOURCES event-il.il)
_mono_special(module-cctor.exe IL SOURCES module-cctor.il)

_mono_special(appdomain-loader.exe SOURCES appdomain-loader.cs
              DEPENDS "${_bin}/appdomain-tester.exe")
_mono_special(event-get.2.exe SOURCES event-get.2.cs DEPENDS "${_bin}/event-il.exe")
_mono_special(module-cctor-loader.2.exe SOURCES module-cctor-loader.2.cs
              DEPENDS "${_bin}/module-cctor.exe")

# --- unhandled exception suite ----------------------------------------------
# Four variants of one test case: two framework versions crossed with the
# legacy and current unhandled-exception policies.
foreach(_v 1 2)
  configure_file("${_src}/unhandled-exception-test-case.2.cs"
                 "${_bin}/unhandled-exception-test-case.${_v}.cs" COPYONLY)
  configure_file("${_src}/unhandled-exception-test-case.2.cs"
                 "${_bin}/unhandled-exception-test-case-legacy.${_v}.cs" COPYONLY)
  configure_file("${_src}/unhandled-exception-base-configuration.config"
                 "${_bin}/unhandled-exception-test-case.${_v}.exe.config" COPYONLY)
  configure_file("${_src}/unhandled-exception-legacy-configuration.config"
                 "${_bin}/unhandled-exception-test-case-legacy.${_v}.exe.config" COPYONLY)
  _mono_special(unhandled-exception-test-case.${_v}.exe
                SOURCES "${_bin}/unhandled-exception-test-case.${_v}.cs")
  _mono_special(unhandled-exception-test-case-legacy.${_v}.exe
                SOURCES "${_bin}/unhandled-exception-test-case-legacy.${_v}.cs")
endforeach()
_mono_special(unhandled-exception-test-runner.2.exe
              SOURCES unhandled-exception-test-runner.2.cs
              DEPENDS "${_bin}/unhandled-exception-test-case.1.exe"
                      "${_bin}/unhandled-exception-test-case.2.exe"
                      "${_bin}/unhandled-exception-test-case-legacy.1.exe"
                      "${_bin}/unhandled-exception-test-case-legacy.2.exe")

# --- InternalsVisibleTo ------------------------------------------------------
# Each case builds the library twice: once permissively so the test compiles,
# then again strictly so the run-time check has something to reject.
function(_mono_internalsvisibleto kind suffix exe_suffix)
  cmake_parse_arguments(ARG "SIGN" "" "" ${ARGN})
  if(ARG_SIGN)
    set(_sign -d:SIGN2048)
    set(_keydep "${_bin}/internalsvisibleto-2048.snk")
  else()
    set(_sign "")
    set(_keydep "")
  endif()
  set(_ok  "internalsvisibleto-correctcase${suffix}.dll")
  set(_bad "internalsvisibleto-wrongcase${suffix}.dll")
  set(_exe "internalsvisibleto-${kind}test${exe_suffix}.exe")
  add_custom_command(
    OUTPUT "${_bin}/${_exe}"
    COMMAND ${_csc} -d:CORRECT_CASE -d:PERMISSIVE ${_sign} -target:library
            "-out:${_bin}/${_ok}" "${_src}/internalsvisibleto-library.cs"
    COMMAND ${_csc} -d:WRONG_CASE -d:PERMISSIVE ${_sign} -target:library
            "-out:${_bin}/${_bad}" "${_src}/internalsvisibleto-library.cs"
    COMMAND ${_csc} -warn:0 ${_sign} "-r:${_bin}/${_ok}" "-r:${_bin}/${_bad}"
            "-out:${_bin}/${_exe}" "${_src}/internalsvisibleto-${kind}test.cs"
    COMMAND ${_csc} -d:CORRECT_CASE ${_sign} -target:library
            "-out:${_bin}/${_ok}" "${_src}/internalsvisibleto-library.cs"
    COMMAND ${_csc} -d:WRONG_CASE ${_sign} -target:library
            "-out:${_bin}/${_bad}" "${_src}/internalsvisibleto-library.cs"
    DEPENDS "${_src}/internalsvisibleto-${kind}test.cs"
            "${_src}/internalsvisibleto-library.cs" ${_keydep} mono-test-toolchain
    WORKING_DIRECTORY "${_bin}"
    COMMENT "CSC ${_exe}"
    VERBATIM)
  set(_all_assemblies "${_all_assemblies};${_bin}/${_exe}" PARENT_SCOPE)
endfunction()

# The compiler-side case uses its own library pair (-2) rather than sharing
# with the run-time case, so the two can be built in parallel.
_mono_internalsvisibleto(runtime  ""             ""          )
_mono_internalsvisibleto(compiler "-2"           ""          )
_mono_internalsvisibleto(runtime  "-sign2048"    "-sign2048" SIGN)
_mono_internalsvisibleto(compiler "-2-sign2048"  "-sign2048" SIGN)
list(APPEND MONO_TESTS_SPECIAL
  internalsvisibleto-runtimetest.exe internalsvisibleto-compilertest.exe
  internalsvisibleto-runtimetest-sign2048.exe internalsvisibleto-compilertest-sign2048.exe)

# --- IgnoresAccessChecksTo ---------------------------------------------------
# Same shape as the InternalsVisibleTo cases above: both libraries are built
# permissively so the test compiles against members it has no right to, then
# rebuilt strictly so the run-time check has something to reject. Only the
# granted one is named by the attribute in the test.
add_custom_command(
  OUTPUT "${_bin}/ignoresaccesschecks-test.exe"
  COMMAND ${_csc} -d:GRANTED -d:PERMISSIVE -t:library
          "-out:${_bin}/ignoresaccesschecks-granted.dll" "${_src}/ignoresaccesschecks-library.cs"
  COMMAND ${_csc} -d:PERMISSIVE -t:library
          "-out:${_bin}/ignoresaccesschecks-ungranted.dll" "${_src}/ignoresaccesschecks-library.cs"
  COMMAND ${_csc} -warn:0 "-r:${_bin}/ignoresaccesschecks-granted.dll"
          "-r:${_bin}/ignoresaccesschecks-ungranted.dll"
          "-out:${_bin}/ignoresaccesschecks-test.exe" "${_src}/ignoresaccesschecks-test.cs"
  COMMAND ${_csc} -d:GRANTED -t:library
          "-out:${_bin}/ignoresaccesschecks-granted.dll" "${_src}/ignoresaccesschecks-library.cs"
  COMMAND ${_csc} -t:library
          "-out:${_bin}/ignoresaccesschecks-ungranted.dll" "${_src}/ignoresaccesschecks-library.cs"
  DEPENDS "${_src}/ignoresaccesschecks-test.cs" "${_src}/ignoresaccesschecks-library.cs"
          mono-test-toolchain
  WORKING_DIRECTORY "${_bin}"
  COMMENT "CSC ignoresaccesschecks-test.exe"
  VERBATIM)
list(APPEND _all_assemblies "${_bin}/ignoresaccesschecks-test.exe")
list(APPEND MONO_TESTS_SPECIAL ignoresaccesschecks-test.exe)

# --- misc --------------------------------------------------------------------
_mono_special(async-exceptions.exe NO_DEFAULT_REFS SOURCES async-exceptions.cs)

# Continuations live in an assembly of their own, and the suites in
# runtime-suites.cmake run this one under named configurations rather than
# alongside the rest of the corpus.
_mono_special(tasklets.exe SOURCES tasklets.cs REFS "${_class}/Mono.Tasklets.dll")

# Deliberately invalid IL, so it is kept out of the corpus every suite runs:
# the runtime-verification tests in runtime-suites.cmake drive it under specific
# --security= and MONO_LLVM_JIT_TIER0 combinations and read what it prints.
_mono_special(verification-invalid-il.exe IL SOURCES verification-invalid-il.il)
_mono_special(verification-inlined-il.exe IL SOURCES verification-inlined-il.il)

# One tailcall test links two extra IL libraries.
_mono_special(tailcall/coreclr/JIT/opt/Tailcall/TailcallVerifyWithPrefix.exe IL
  SOURCES tailcall/coreclr/JIT/opt/Tailcall/TailcallVerifyWithPrefix.il
          tailcall/coreclr/JIT/opt/Tailcall/TailcallVerifyTransparentLibraryWithPrefix.il
          tailcall/coreclr/JIT/opt/Tailcall/TailcallVerifyVerifiableLibraryWithPrefix.il)
