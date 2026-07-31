# The class-library test suites.
#
# Two frameworks, side by side, exactly as the assemblies are written for:
# NUnit assemblies (<Assembly>_test.dll.sources, run by nunit-lite-console out
# of this tree) and xunit ones (<Assembly>_xtest.dll.sources, run by the
# prebuilt console in external/xunit-binaries).  A directory can have both.
#
# Each becomes one CTest test, labelled `bcl` or `bcl-xunit`.  The assemblies
# are built by the regular build -- the mcs-tests and mcs-xunit-tests
# aggregates are in `all` -- so running ctest never builds anything.

include_guard(GLOBAL)

set(MONO_TEST_XUNIT_DIR "${CMAKE_SOURCE_DIR}/external/xunit-binaries")

# Traits that mark a test as not-for-us.  `failing` and `nonmonotests` are the
# assemblies' own opt-outs; `outerloop` is the long tail corefx excludes by
# default.
set(MONO_TEST_XUNIT_NOTRAITS
    category=failing category=nonmonotests Benchmark=true
    category=outerloop category=nonlinuxtests)

# Categories nunit-lite is told to skip.  CAS is the code-access-security
# suite, which this runtime does not implement.
set(MONO_TEST_NUNIT_EXCLUDES NotWorking CAS)

# Suites whose source list names a file that is not in the tree.  System's
# xunit list wants
# external/corefx-bugfix/src/System.IO.FileSystem.Watcher/tests/Args.FileSystemEventArgs.cs,
# which the commit that redirected it there never added to the overlay.  Drop
# the entry from this list once that file exists.
set(MONO_TEST_XUNIT_UNBUILDABLE System)

# Where a directory's test fixtures -- helper assemblies a suite embeds as a
# resource -- are built.  A directory produces them from its extra.cmake.
function(mono_test_fixture_dir out profile assembly)
  _mono_stem(_stem "${assembly}")
  set(${out} "${MONO_MANAGED_DEPSDIR}/testfixtures/${profile}/${_stem}" PARENT_SCOPE)
endfunction()

# Points the -resource: flags that name a fixture at where it is built.  A
# resource path that does not exist in the source tree is one: the suites embed
# helper assemblies they also compile.
function(_mono_rewrite_fixture_flags out flags dir fixdir)
  set(_result "")
  foreach(_f IN LISTS flags)
    if(_f MATCHES "^([-/]resource:)([^,]+)(,?.*)$")
      set(_pfx "${CMAKE_MATCH_1}")
      set(_rpath "${CMAKE_MATCH_2}")
      set(_rrest "${CMAKE_MATCH_3}")
      if(NOT IS_ABSOLUTE "${_rpath}" AND NOT EXISTS "${dir}/${_rpath}")
        get_filename_component(_rname "${_rpath}" NAME)
        if(_rrest STREQUAL "")
          set(_rrest ",${_rname}")
        endif()
        set(_f "${_pfx}${fixdir}/${_rname}${_rrest}")
      endif()
    endif()
    list(APPEND _result "${_f}")
  endforeach()
  set(${out} "${_result}" PARENT_SCOPE)
endfunction()

# Compiles a fixture assembly for one directory's suite.  Called from that
# directory's extra.cmake, before the suite is materialized.
#
#   mono_test_fixture_assembly(PROFILE <p> ASSEMBLY <lib.dll> NAME <fixture.dll>
#                              SOURCES <file>... [REFS <assembly>...]
#                              [PROGRAM] [IN_TESTS_DIR] [FLAGS <flag>...])
#
# IN_TESTS_DIR puts it beside the test assemblies instead, for the suites that
# find it by looking next to themselves.
function(mono_test_fixture_assembly)
  cmake_parse_arguments(F "PROGRAM;IN_TESTS_DIR" "PROFILE;ASSEMBLY;NAME"
                        "SOURCES;REFS;FLAGS" ${ARGN})
  mono_profile_dir(_pdir ${F_PROFILE})
  mono_test_fixture_dir(_dir ${F_PROFILE} "${F_ASSEMBLY}")
  if(F_IN_TESTS_DIR)
    set(_dir "${_pdir}/tests")
  endif()
  _mono_stem(_stem "${F_NAME}")
  _mono_stem(_astem "${F_ASSEMBLY}")
  set(_out "${_dir}/${F_NAME}")
  set(_target_kind -target:library)
  if(F_PROGRAM)
    set(_target_kind -target:exe)
  endif()

  set(_refflags "")
  set(_deps "")
  foreach(_r IN LISTS F_REFS ITEMS mscorlib)
    list(APPEND _refflags "-r:${_pdir}/${_r}.dll")
    get_property(_p GLOBAL PROPERTY MONO_MANAGED_PROVIDER_${F_PROFILE}/${_r})
    if(_p)
      list(APPEND _deps "${_p}" "${_pdir}/${_r}.dll")
    endif()
  endforeach()

  set(_srcs "")
  foreach(_s IN LISTS F_SOURCES)
    get_filename_component(_s "${_s}" ABSOLUTE)
    list(APPEND _srcs "${_s}")
  endforeach()

  _mono_csc_command(_csc ${F_PROFILE})
  _mono_csc_env(_env ${F_PROFILE})
  _mono_tool_depends(_rt ${F_PROFILE})
  set(_cmd ${_csc})
  if(_env)
    set(_cmd "${CMAKE_COMMAND}" -E env ${_env} ${_csc})
  endif()

  set(_target "mcs-${F_PROFILE}-${_astem}-fixture-${_stem}")
  add_custom_command(
    OUTPUT "${_out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
    COMMAND ${_cmd} /nologo /noconfig -nostdlib ${_target_kind}
            ${MONO_MANAGED_DEBUG_FLAGS} ${F_FLAGS} ${_refflags}
            "-out:${_out}" ${_srcs}
    DEPENDS ${_srcs} ${_deps} ${_rt}
    COMMENT "CSC [${F_PROFILE}] fixture ${F_NAME}"
    VERBATIM)
  add_custom_target(${_target} DEPENDS "${_out}")
  set_property(GLOBAL APPEND PROPERTY
               MONO_TEST_FIXTURES_${F_PROFILE}_${_astem} ${_target})
endfunction()

# Extra environment and build dependencies for one directory's suite, set from
# that directory's extra.cmake.  The xbuild suites use it to point xbuild at
# the staged target tree they run against.
#
#   mono_test_environment(PROFILE <p> ASSEMBLY <lib.dll>
#                         [ENVIRONMENT <VAR=value>...] [DEPENDS <target>...])
function(mono_test_environment)
  cmake_parse_arguments(E "" "PROFILE;ASSEMBLY" "ENVIRONMENT;DEPENDS" ${ARGN})
  _mono_stem(_stem "${E_ASSEMBLY}")
  if(E_ENVIRONMENT)
    set_property(GLOBAL APPEND PROPERTY
                 MONO_TEST_ENV_${E_PROFILE}_${_stem} ${E_ENVIRONMENT})
  endif()
  if(E_DEPENDS)
    set_property(GLOBAL APPEND PROPERTY
                 MONO_TEST_FIXTURES_${E_PROFILE}_${_stem} ${E_DEPENDS})
  endif()
endfunction()

# Locates one app.config fragment.  The xbuild suites' fragment is generated
# from a template rather than checked in, because it names the assembly version
# their binding redirects point at.
function(_mono_test_config_fragment out profile dir name)
  if(EXISTS "${dir}/${name}")
    set(${out} "${dir}/${name}" PARENT_SCOPE)
    return()
  endif()
  set(ASM_VERSION "${MONO_PROFILE_${profile}_XBUILD_VERSION}.0.0")
  set(_gen "${MONO_MANAGED_DEPSDIR}/xbuild-test-config-${profile}.xml")
  configure_file("${MONO_MCS_TOPDIR}/tools/xbuild/data/xbuild.exe.config_test.in"
                 "${_gen}" @ONLY)
  set(${out} "${_gen}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The per-test runner directory
# ---------------------------------------------------------------------------
# nunit-lite-console reads its own app.config, and several libraries need to
# add to it -- System wants trace switches, the xbuild ones want binding
# redirects.  Each test therefore gets its own copy of the runner so the
# configs cannot collide when ctest runs them in parallel.
function(_mono_test_runner out target profile dir global runtime files)
  mono_profile_dir(_pdir ${profile})
  set(_rundir "${_pdir}/tests/runner/${target}")
  set(_exe "${_rundir}/nunit-lite-console.exe")
  set(_tmpl
      "${MONO_MCS_TOPDIR}/tools/nunit-lite/nunit-lite-console/nunit-lite-console.exe.config.tmpl")

  # The template carries two comment markers; each fragment is spliced in
  # after the one it names.
  set(_global_text "")
  set(_runtime_text "")
  if(global)
    _mono_test_config_fragment(_f ${profile} "${dir}" "${global}")
    file(READ "${_f}" _global_text)
  endif()
  if(runtime)
    _mono_test_config_fragment(_f ${profile} "${dir}" "${runtime}")
    file(READ "${_f}" _runtime_text)
  endif()
  file(READ "${_tmpl}" _cfg)
  string(REPLACE "<!-- __INSERT_CUSTOM_APP_CONFIG_GLOBAL__ -->" "${_global_text}" _cfg "${_cfg}")
  string(REPLACE "<!-- __INSERT_CUSTOM_APP_CONFIG_RUNTIME__ -->" "${_runtime_text}" _cfg "${_cfg}")
  file(GENERATE OUTPUT "${_exe}.config" CONTENT "${_cfg}")

  # A `<src>,<name>` pair puts a file beside the runner under a name of its own.
  # The config above can point at one: an <appSettings file="..."/> is resolved
  # relative to the process, which is the runner, not the test assembly.
  set(_extra "")
  set(_extradeps "")
  foreach(_f IN LISTS files)
    if(NOT _f MATCHES "^([^,]+),(.+)$")
      message(FATAL_ERROR "TEST_RUNNER_FILES wants <src>,<name>, got '${_f}'")
    endif()
    set(_fsrc "${CMAKE_MATCH_1}")
    if(NOT IS_ABSOLUTE "${_fsrc}")
      set(_fsrc "${dir}/${_fsrc}")
    endif()
    list(APPEND _extra COMMAND "${CMAKE_COMMAND}" -E copy_if_different
         "${_fsrc}" "${_rundir}/${CMAKE_MATCH_2}")
    list(APPEND _extradeps "${_fsrc}")
  endforeach()

  _mono_target_name(_console ${profile} "" nunit-lite-console.exe)
  add_custom_command(
    OUTPUT "${_exe}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_pdir}/nunit-lite-console.exe" "${_exe}"
    ${_extra}
    DEPENDS ${_console} "${_pdir}/nunit-lite-console.exe" ${_extradeps}
    COMMENT "COPY [${profile}] runner for ${target}"
    VERBATIM)
  set(${out} "${_exe}" PARENT_SCOPE)
endfunction()

# xunit.console loads the execution engine from beside the assembly under
# test, not from its own directory, so the two runtime halves are copied there.
function(_mono_xunit_runtime out profile)
  mono_profile_dir(_pdir ${profile})
  set(_dir "${_pdir}/tests")
  set(_copied "")
  foreach(_d Xunit.NetCore.Extensions xunit.execution.dotnet)
    list(APPEND _copied "${_dir}/${_d}.dll")
  endforeach()
  set(${out} ${_copied} PARENT_SCOPE)
  if(TARGET mcs-${profile}-xunit-runtime)
    return()
  endif()
  set(_cmds "")
  foreach(_d Xunit.NetCore.Extensions xunit.execution.dotnet)
    list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy_if_different
         "${MONO_TEST_XUNIT_DIR}/${_d}.dll" "${_dir}/${_d}.dll")
  endforeach()
  add_custom_command(
    OUTPUT ${_copied}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
    ${_cmds}
    COMMENT "COPY [${profile}] xunit runtime"
    VERBATIM)
  add_custom_target(mcs-${profile}-xunit-runtime DEPENDS ${_copied})
endfunction()

# The second process a RemoteExecutor test starts.  One per profile, built on
# first use.
function(_mono_remote_executor out profile)
  mono_profile_dir(_pdir ${profile})
  set(_exe "${_pdir}/tests/RemoteExecutorConsoleApp.exe")
  set(${out} "${_exe}" PARENT_SCOPE)
  if(TARGET mcs-${profile}-remote-executor)
    return()
  endif()
  set(_src
      "${CMAKE_SOURCE_DIR}/external/corefx/src/Common/tests/System/Diagnostics/RemoteExecutorConsoleApp/RemoteExecutorConsoleApp.cs")
  _mono_csc_command(_csc ${profile})
  _mono_csc_env(_env ${profile})
  _mono_tool_depends(_rt ${profile})
  set(_cmd ${_csc})
  if(_env)
    set(_cmd "${CMAKE_COMMAND}" -E env ${_env} ${_csc})
  endif()
  get_property(_p GLOBAL PROPERTY MONO_MANAGED_PROVIDER_${profile}/mscorlib)
  add_custom_command(
    OUTPUT "${_exe}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_pdir}/tests"
    COMMAND ${_cmd} /nologo /noconfig -nostdlib
            "-r:${_pdir}/mscorlib.dll" "-out:${_exe}" "${_src}"
    DEPENDS "${_src}" ${_p} "${_pdir}/mscorlib.dll" ${_rt}
    COMMENT "CSC [${profile}] RemoteExecutorConsoleApp.exe"
    VERBATIM)
  add_custom_target(mcs-${profile}-remote-executor DEPENDS "${_exe}")
endfunction()

# ---------------------------------------------------------------------------
# One test assembly
# ---------------------------------------------------------------------------
# Called from _mono_materialize_profile() once the library it tests exists, so
# every A_* field of the declaration is still in scope.  `kind` is nunit or
# xunit; the two differ in the source list, the references and the runner, but
# not in shape.
# `stem` is what the suite is named after, which is not always the assembly's
# own name: corlib builds mscorlib.dll and Cscompmgd builds cscompmgd.dll, and
# their suites keep the declared spelling -- corlib_test.dll, and so on.
function(_mono_add_managed_test kind profile dir target assembly stem sources_file)
  cmake_parse_arguments(T "" "GLOBAL_CONFIG;RUNTIME_CONFIG;REMOTE_EXECUTOR"
                        "REFS;FLAGS;DEPENDS;LIB_REFFLAGS;RESOURCES;EXCLUDES;RUNNER_FILES"
                        ${ARGN})

  mono_profile_dir(_pdir ${profile})
  set(_testdir "${_pdir}/tests")
  set(_stem "${stem}")
  # The fixtures and the extra environment are registered from a directory's
  # extra.cmake, which names the assembly it can see -- mscorlib.dll, not
  # corlib.dll -- so those two are keyed on that instead.
  _mono_stem(_astem "${assembly}")

  if(kind STREQUAL xunit AND _stem IN_LIST MONO_TEST_XUNIT_UNBUILDABLE)
    return()
  endif()

  if(kind STREQUAL nunit)
    set(_name "${profile}_${_stem}_test.dll")
    set(_library "${_stem}_test.dll")
  else()
    set(_name "${profile}_${_stem}_xunit-test.dll")
    set(_library "${_stem}_xtest.dll")
  endif()
  set(_out "${_testdir}/${_name}")
  set(_testtarget "${target}-${kind}-test")

  # -- references ----------------------------------------------------------
  # The test assembly gets the library's own references as well as its
  # TEST_LIB_REFS: it compiles against that library's surface, so anything the
  # library exposes has to be resolvable here too.
  set(_refflags "-r:${_pdir}/${assembly}" ${T_LIB_REFFLAGS})
  set(_refdeps "${target}" "${_pdir}/${assembly}")
  set(_refs ${T_REFS} ${MONO_PROFILE_${profile}_DEFAULT_REFERENCES})
  foreach(_r IN LISTS _refs)
    list(APPEND _refflags "-r:${_pdir}/${_r}.dll")
    get_property(_p GLOBAL PROPERTY MONO_MANAGED_PROVIDER_${profile}/${_r})
    if(_p)
      list(APPEND _refdeps "${_p}" "${_pdir}/${_r}.dll")
    endif()
  endforeach()

  list(REMOVE_DUPLICATES _refflags)

  get_property(_fixtures GLOBAL PROPERTY MONO_TEST_FIXTURES_${profile}_${_astem})
  if(_fixtures)
    list(APPEND _refdeps ${_fixtures})
  endif()

  set(_extra_sources "")
  if(kind STREQUAL nunit)
    list(APPEND _refflags "-r:${_pdir}/nunitlite.dll")
    _mono_target_name(_nl ${profile} "" nunitlite.dll)
    list(APPEND _refdeps ${_nl} "${_pdir}/nunitlite.dll")
  else()
    foreach(_r xunit.core xunit.execution.dotnet xunit.abstractions xunit.assert
               Xunit.NetCore.Extensions)
      list(APPEND _refflags "-r:${MONO_TEST_XUNIT_DIR}/${_r}.dll")
    endforeach()
    foreach(_r netstandard System.Runtime)
      list(APPEND _refflags "-r:${_pdir}/Facades/${_r}.dll")
      get_property(_p GLOBAL PROPERTY MONO_MANAGED_PROVIDER_${profile}/Facades/${_r})
      if(_p)
        list(APPEND _refdeps "${_p}")
      endif()
    endforeach()
    # Compiled in rather than referenced: the xunit assemblies expect these
    # types in their own assembly.
    set(_extra_sources
        "${MONO_TEST_XUNIT_DIR}/BenchmarkAttribute.cs"
        "${MONO_TEST_XUNIT_DIR}/BenchmarkDiscover.cs"
        "${MONO_MCS_TOPDIR}/class/test-helpers/PlatformDetection.cs")
    # Suites that run a fragment of themselves in a second process compile in
    # corefx's harness for it, and need the helper it starts.
    if(T_REMOTE_EXECUTOR)
      set(_corefx "${CMAKE_SOURCE_DIR}/external/corefx/src")
      list(APPEND _extra_sources
           "${MONO_MCS_TOPDIR}/class/test-helpers/AdminHelper.cs"
           "${_corefx}/CoreFx.Private.TestUtilities/src/System/IO/FileCleanupTestBase.cs"
           "${_corefx}/CoreFx.Private.TestUtilities/src/System/Diagnostics/RemoteExecutorTestBase.cs"
           "${_corefx}/Common/src/System/PasteArguments.cs"
           "${_corefx}/Common/src/System/PasteArguments.Unix.cs"
           "${MONO_MCS_TOPDIR}/class/test-helpers/RemoteExecutorTestBase.Mono.cs"
           "${_corefx}/CoreFx.Private.TestUtilities/src/System/Diagnostics/RemoteExecutorTestBase.Process.cs")
      _mono_remote_executor(_remote ${profile})
      list(APPEND _refdeps "${_remote}")
    endif()
  endif()

  # -- compile -------------------------------------------------------------
  set(_response "${MONO_MANAGED_DEPSDIR}/${profile}_${_library}.response")
  set(_depfile  "${MONO_MANAGED_DEPSDIR}/${_testtarget}.d")
  set(_settings "${MONO_MANAGED_DEPSDIR}/${_testtarget}.cmake")

  _mono_tool_command(_gensources ${profile} gensources.exe)
  _mono_tool_env(_tool_env ${profile})
  _mono_csc_command(_csc ${profile})
  _mono_csc_env(_csc_env ${profile})
  _mono_tool_depends(_rt ${profile})
  get_property(_gs GLOBAL PROPERTY MONO_MANAGED_PROVIDER_build/gensources)

  set(_flags ${MONO_MANAGED_COMMON_FLAGS} ${MONO_PROFILE_${profile}_MCS_FLAGS}
             ${MONO_MANAGED_DEBUG_FLAGS} -target:library
             ${T_FLAGS} ${_refflags})
  if(MONO_MCS_COMPILER_SERVER)
    list(APPEND _flags "/shared:${MONO_MCS_PIPENAME}")
  endif()
  if(kind STREQUAL xunit)
    list(APPEND _flags /unsafe)
  endif()

  # System.Runtime.Serialization embeds 522 schema files.  They go in a
  # response file because that many -resource: flags overflow the command line.
  if(T_RESOURCES)
    set(_reslines "")
    foreach(_r IN LISTS T_RESOURCES)
      list(APPEND _reslines "-resource:${_r},${_r}")
    endforeach()
    string(JOIN "\n" _restext ${_reslines})
    set(_resrsp "${MONO_MANAGED_DEPSDIR}/${_testtarget}.resources.rsp")
    file(GENERATE OUTPUT "${_resrsp}" CONTENT "${_restext}\n")
    list(APPEND _flags "@${_resrsp}")
  endif()

  # gensources resolves the nunit lists against Test/, which is where the
  # per-library suites live; the xunit lists spell their paths in full.
  set(_basedir "")
  if(kind STREQUAL nunit)
    set(_basedir "--basedir:${dir}/Test")
  endif()

  set(_prof "${profile}")
  set(_platform "linux")
  if(NOT MONO_PROFILE_${profile}_ALIAS)
    set(_platform "")
  endif()
  set(_sources_inputs ${sources_file})

  file(CONFIGURE OUTPUT "${_settings}" CONTENT [[
# Generated by MonoManagedTests.cmake -- do not edit.
set(MCS_SOURCE_DIR    [==[@dir@]==])
set(MCS_OUTPUT        [==[@_out@]==])
set(MCS_BUILD_OUTPUT  [==[@_out@]==])
set(MCS_DEPFILE       [==[@_depfile@]==])
set(MCS_RESPONSE      [==[@_response@]==])
set(MCS_SOURCES_INPUTS [==[@_sources_inputs@]==])
set(MCS_GENSOURCES    [==[@_gensources@]==])
set(MCS_GENSOURCES_BASEDIR [==[@_basedir@]==])
set(MCS_PLATFORM_NAMES [==[@MONO_MANAGED_PLATFORM_NAMES@]==])
set(MCS_PROFILE_NAMES [==[@MONO_MANAGED_PROFILES@]==])
set(MCS_LIBRARY       [==[@_library@]==])
set(MCS_PLATFORM      [==[@_platform@]==])
set(MCS_PROFILE       [==[@_prof@]==])
set(MCS_CSC           [==[@_csc@]==])
set(MCS_CSC_FLAGS     [==[@_flags@]==])
set(MCS_CSC_ENV       [==[@_csc_env@]==])
set(MCS_TOOL_ENV      [==[@_tool_env@]==])
set(MCS_BUILT_SOURCES [==[@_extra_sources@]==])
]] @ONLY)

  add_custom_command(
    OUTPUT "${_out}"
    COMMAND "${CMAKE_COMMAND}" -D "SETTINGS=${_settings}"
            -P "${CMAKE_SOURCE_DIR}/cmake/MonoCompileAssembly.cmake"
    DEPENDS ${sources_file} ${_refdeps} ${_rt} ${_gs} ${T_DEPENDS} "${_settings}"
    DEPFILE "${_depfile}"
    COMMENT "CSC [${profile}] ${_name}"
    VERBATIM)

  # -- run -----------------------------------------------------------------
  set(_testname "bcl-${_stem}")
  set(_deps "${_out}")
  if(kind STREQUAL nunit)
    _mono_test_runner(_runner "${_stem}" ${profile} "${dir}"
                      "${T_GLOBAL_CONFIG}" "${T_RUNTIME_CONFIG}"
                      "${T_RUNNER_FILES}")
    list(APPEND _deps "${_runner}")
    string(JOIN "," _excludes ${MONO_TEST_NUNIT_EXCLUDES} ${T_EXCLUDES})
    set(_cmd "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" --debug "${_runner}"
             "${_out}" "-exclude=${_excludes}" -format:nunit2
             "-result:${_testdir}/TestResult-${profile}-${_stem}.xml")
    set(_mono_path "${_pdir}:${_testdir}:${dir}")
  else()
    set(_testname "bcl-xunit-${_stem}")
    set(_cmd "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" --debug
             "${MONO_TEST_XUNIT_DIR}/xunit.console.exe" "${_out}"
             -noappdomain -noshadow -parallel none
             -nunit "${_testdir}/TestResult-${profile}-${_stem}-xunit.xml")
    foreach(_t IN LISTS MONO_TEST_XUNIT_NOTRAITS)
      list(APPEND _cmd -notrait "${_t}")
    endforeach()
    _mono_xunit_runtime(_xrt ${profile})
    list(APPEND _deps ${_xrt})
    set(_mono_path "${_pdir}:${_testdir}:${dir}:${MONO_TEST_XUNIT_DIR}")
    if(T_REMOTE_EXECUTOR)
      _mono_remote_executor(_remote ${profile})
      set(_env_extra "REMOTE_EXECUTOR=${_remote}")
    endif()
  endif()

  get_property(_env_dir GLOBAL PROPERTY MONO_TEST_ENV_${profile}_${_astem})
  if(_env_dir)
    string(JOIN ";" _env_dir ${_env_dir})
    set(_env_dir ";${_env_dir}")
  endif()

  add_custom_target(${_testtarget} DEPENDS ${_deps})
  if(kind STREQUAL nunit)
    add_dependencies(mcs-tests ${_testtarget})
    set(_label bcl)
  else()
    add_dependencies(mcs-xunit-tests ${_testtarget})
    set(_label bcl-xunit)
  endif()

  # LD_LIBRARY_PATH is for the profiler suite, which re-execs the runtime with
  # --profile=log and needs it to find the module this build produced.
  #
  # PATH leads with the wrapper shims, because a suite that compiles code at run
  # time -- System.CodeDom, and the Csc and Vbc tasks the xbuild suites drive --
  # spawns `csc` or `mcs` by bare name and has to reach this tree's rather than
  # whatever the distribution installed.
  #
  # The suites read fixture files by paths relative to their own directory,
  # hence the working directory.
  #
  # fx_<suite> has no setup half -- the assemblies come from the regular build.
  # It exists so a directory can register a FIXTURES_CLEANUP against it (the
  # Mono.Debugger.Soft sweep) and have it ordered after the suite.
  add_test(NAME ${_testname} COMMAND ${_cmd} WORKING_DIRECTORY "${dir}")
  set_tests_properties(${_testname} PROPERTIES
    LABELS "${_label}"
    TIMEOUT 1800
    FIXTURES_REQUIRED fx_${_testname}
    ENVIRONMENT "MONO_PATH=${_mono_path};MONO_REGISTRY_PATH=$ENV{HOME}/.mono/registry;MONO_TESTS_IN_PROGRESS=yes;PATH=${CMAKE_BINARY_DIR}/runtime/_tmpinst/bin:$ENV{PATH};LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/mono/profiler:$ENV{LD_LIBRARY_PATH};${_env_extra}${_env_dir}")
endfunction()

# Finds the .sources file for a test assembly, preferring the profile-specific
# spelling, and returns empty if the directory has no such suite.
# The list is named after the assembly as the directory declares it, which is
# not always what the assembly is called: corlib builds mscorlib.dll and
# Cscompmgd builds cscompmgd.dll, and both keep their declared name here.  So
# both spellings are tried.
function(_mono_test_sources out out_stem kind profile dir assembly declared)
  # The prefixes tests.make looks for: the profile for a NUnit list, the
  # platform and the profile for an xunit one.
  if(kind STREQUAL nunit)
    set(_suffix "_test.dll.sources")
    set(_prefix "${profile}_")
  else()
    set(_suffix "_xtest.dll.sources")
    set(_prefix "linux_${profile}_")
    if(NOT MONO_PROFILE_${profile}_ALIAS)
      set(_prefix "${profile}_")
    endif()
  endif()
  foreach(_n "${assembly}" "${declared}")
    _mono_stem(_stem "${_n}")
    foreach(_c "${_prefix}${_stem}${_suffix}" "${_stem}${_suffix}")
      if(EXISTS "${dir}/${_c}")
        set(${out} "${dir}/${_c}" PARENT_SCOPE)
        # The stem the list is named after is also the name gensources has to
        # be asked for, and the name the test assembly itself takes.
        set(${out_stem} "${_stem}" PARENT_SCOPE)
        return()
      endif()
    endforeach()
  endforeach()
  set(${out} "" PARENT_SCOPE)
  set(${out_stem} "" PARENT_SCOPE)
endfunction()
