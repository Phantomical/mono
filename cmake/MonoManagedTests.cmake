# The class-library test suites.
#
# Two frameworks, side by side, exactly as the assemblies are written for:
# NUnit assemblies (<Assembly>_test.dll.sources, run by nunit-lite-console out
# of this tree) and xunit ones (<Assembly>_xtest.dll.sources, run by the
# prebuilt console in external/xunit-binaries).  A directory can have both.
#
# Each becomes one CTest test, labelled `bcl` or `bcl-xunit`, except for the
# suites in MONO_BCL_TESTS_SPLIT, which become one test per namespace.  The
# assemblies are built by the regular build -- the mcs-tests and mcs-xunit-tests
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

# Tests that reach a service outside the machine: `InetAccess` for the ones that
# resolve a hostname and connect, and the whole of System.Messaging, which is an
# integration suite against a broker on localhost.  Off by default, because on a
# box without them these do not report a runtime defect - they report the
# network - and a permanently red suite is one nobody reads.
#
# Turning it on runs them; there is nothing here that deletes them.
option(MONO_ENABLE_NETWORK_TESTS "Run tests that need an external network or message broker" OFF)
if(NOT MONO_ENABLE_NETWORK_TESTS)
  list(APPEND MONO_TEST_NUNIT_EXCLUDES InetAccess)
endif()

# Likewise for a display. Kept separate from the network option because a
# machine can easily have one and not the other.
option(MONO_ENABLE_GUI_TESTS "Run tests that need an X display" OFF)

# What one suite gets to run for.  A suite is a whole assembly's worth of cases
# driven by one console, so the budget is per-assembly and generous by the
# standards of the rest of the tree.
set(MONO_BCL_TEST_TIMEOUT 1800)
set(MONO_BCL_TEST_LONG_TIMEOUT 3600)

# The suites that do not fit MONO_BCL_TEST_TIMEOUT.  Both of these have been
# seen finishing and been killed at the limit on different sweeps of the same
# tree, which is the shape of a budget that is too tight rather than of a test
# that hangs -- a suite that wedges never reports a time at all.
set(MONO_BCL_TESTS_LONG
  # The largest assembly in the tree by case count, and the one whose cost
  # swings most with machine load: 868s, 1674s, 1722s and a 1792s pass measured
  # across four runs, against the 1800s budget it then timed out on.
  bcl-xunit-corlib
  # 224s on an idle machine and 1541s on a busy one, and killed at 1800s on
  # three separate sweeps.  Most of that spread is the XSLT and schema
  # fixtures, which are compute-bound and get whatever share of the cores is
  # left.
  bcl-System.Xml
  # MonoTests.System.Web.UI.WebControls takes 1854s with the machine to itself.
  # It spends that forking a C# compiler per page, so the console itself sits
  # near idle while the run as a whole holds about one core throughout -- more
  # cores would not shorten it, it just needs the wall time.
  bcl-System.Web
  # Its `rest` group is nearly the whole assembly in one console: 733s at four
  # threads on a quiet machine, against 1631s for the Expressions namespace
  # alone when every collection ran in turn.  733s would fit the standard 1800s,
  # but this is the suite whose measurements swing most with load, and a run
  # that has to share the cores loses the threads before it loses anything else.
  bcl-xunit-System.Core
)

# ---------------------------------------------------------------------------
# Splitting a suite up
# ---------------------------------------------------------------------------
# A whole assembly under one console is opaque while it runs: nothing is printed
# between the banner and the summary, so a suite that wedges reports a timeout
# and nothing else -- the same output whether it died on its first fixture or
# its last.  The suites below are cut into one CTest test per namespace instead,
# which gives a wedge a name, keeps one bad fixture from taking every other
# result in the assembly with it, and lets a suite use more than one core.
#
# The listing is done at build time by MonoBclDiscover.cmake, once per build of
# the assembly rather than once per ctest run.  A suite that cannot be listed
# stays whole -- see that file.
#
# Splitting is worth it in proportion to what a suite costs, because each group
# pays for a process of its own.  Measured on this tree with the machine busy:
# nunit-lite-console needs ~5.5s before it runs a single case (~6.9s against
# corlib's test assembly), essentially all of it JIT of the runner, and
# xunit.console needs ~41s (~49s against corlib's).  So a twenty-second suite
# would be slower split than whole, and the xunit half is capped rather than cut
# per namespace.
set(MONO_BCL_TESTS_SPLIT
  # Killed at 1800s on every sweep this tree has recorded, with nothing to say
  # where.  550 fixtures over 56 namespaces.
  bcl-corlib
  # 512 fixtures, 7190 cases -- second largest nunit assembly.
  bcl-System
  # MONO_BCL_TESTS_LONG: 224s idle, 1541s loaded.  Most of the spread is the
  # XSLT and schema fixtures, which land in two namespaces out of eleven.
  bcl-System.Xml
  # 514 fixtures and near its budget either way (1583s completing, 1801s
  # timeouts), so the parallelism is what it needs most.
  bcl-System.Web
  bcl-System.Data
  bcl-System.Core
  # Also killed at 1800s on every recorded sweep.  168 fixtures, 10 namespaces.
  bcl-System.ServiceModel
  # MONO_BCL_TESTS_LONG, and the largest assembly in the tree.
  bcl-xunit-corlib
  # Killed at 1800s on every recorded sweep.
  bcl-xunit-System.Core
)

# `bcl-Mono.Debugger.Soft` is not here and cannot be: it is one fixture holding
# all 124 of its cases, so there is nothing to cut along.

# How many groups a split suite is allowed.  The nunit half is uncapped -- 56
# groups for corlib costs about 6 minutes of CPU it did not spend before, and
# buys 56-way parallelism on a suite that has never finished.  The xunit half
# cannot afford that: 28 namespaces at ~45s of startup each would be most of an
# hour of CPU, so the heaviest namespaces get a group and the rest share one.
set(MONO_BCL_SPLIT_MAX_GROUPS_nunit 0)
set(MONO_BCL_SPLIT_MAX_GROUPS_xunit 8)

# Namespaces that must not share a console with the rest of their assembly, as
# `<suite>/<namespace>`.  Naming any changes what splitting that suite means:
# instead of cutting it into groups by weight, the named namespaces get a
# process each and everything else runs in one, as their complement.
# MONO_BCL_SPLIT_MAX_GROUPS no longer applies to it.
#
# That is the shape a suite wants once it has an entry in
# MONO_BCL_TESTS_XUNIT_THREADS.  Splitting exists to buy parallelism the console
# would not otherwise have, and in-process parallelism is the cheaper way to buy
# it: one console at four threads runs the same cases as four consoles at one,
# without paying ~45s of runner JIT four times, and it fills those threads from
# every class in the assembly rather than from one namespace at a time.
#
# The price is the other thing splitting bought.  A wedge in `rest` reports a
# timeout naming `rest`, which for an assembly with one quarantined namespace is
# barely narrower than naming the assembly -- so this trades diagnosis for wall
# clock, and is worth it only where the wall clock is the problem.  Suites
# without a MONO_BCL_TESTS_XUNIT_THREADS entry should stay split by weight.
#
# The complement is the part that has to be right.  A suite cut this way has no
# catch-all, so `rest` is spelled as `-noclass` over the quarantined classes
# rather than `-class` over the others: a class the lister missed then runs in
# `rest` instead of running nowhere.
set(MONO_BCL_TESTS_XUNIT_SERIAL
  # PLINQ starts a query's worth of thread-pool work per case, so it is both the
  # namespace most likely to be disturbed by neighbours and the one most likely
  # to disturb them.  This is what upstream's assembly-wide CollectionBehavior
  # was protecting before System.Core_xtest.dll.sources dropped it; giving the
  # namespace a process keeps the protection without pinning the other 300
  # classes in the assembly to one thread.
  bcl-xunit-System.Core/System.Linq.Parallel.Tests
)

# A group runs under its assembly's budget rather than one of its own, so that
# splitting a suite can never leave a test with less time than it had when the
# suite ran whole.  There is deliberately no group timeout variable: the template
# hands every test the same `_timeout`, which makes the invariant structural
# rather than something the next person to touch this has to remember.
#
# Any fraction of the assembly's budget would be a bet on the cases being spread
# evenly across namespaces, and they are not -- System.Linq.Expressions.Tests is
# most of bcl-xunit-System.Core on its own.  Nor can cutting a suite finer than
# namespaces recover a smaller budget: a slow group here is usually slow because
# of one fixture, not many.  MonoTests.System.Web.Compilation is 700s of which
# TemplateControlCompilerTest is 555, and MonoTests.System is 96 fixtures of
# which AppDomainTest is 82% of the time -- and a fixture is the finest cut the
# runners offer.
#
# Raising the budget is not the same as being happy about what it buys, which is
# what MONO_BCL_TESTS_SLOW below is for.
#
# The cost is that a wedged group burns the assembly's full budget, which is no
# better than before the split -- but no worse either, and the diagnostic the
# split was for is untouched: the timeout names the group, and the assembly's
# other groups still run and still report.

# The class-library tests that cost minutes rather than seconds.  These carry
# `slow` on top of their usual label, so `check` and `check-all` skip them while
# `-L bcl`, `-R <name>` and a nightly that runs the label still get them.  They
# are not disabled: this is the highest-signal coverage on this branch -- runtime
# codegen, Expression.Compile, appdomain teardown, the soft debugger -- and their
# cost is partly the JIT's own per-method floor, so a run that got slower is a
# result rather than a nuisance.
#
# A name here is a CTest test name: `<suite>` for an unsplit suite, and
# `<suite>/<group>` for one that is cut up.  Re-listing an assembly can rename
# its groups, so an entry that matches nothing warns at configure time rather
# than going quietly inert.
set(MONO_BCL_TESTS_SLOW
  # 1553s, passing, measured on its own against a 3600s budget.  Nearly all of
  # it is XmlSerializer building temporary serializer assemblies, which forks a
  # C# compiler that then runs on the runtime being built -- so this suite reads
  # the JIT's startup cost several hundred times over.
  bcl-System.Xml/MonoTests.System.XmlSerialization
  # 271 classes, and the only test here whose cost is genuinely spread rather
  # than concentrated in one fixture -- which is what makes it the one that
  # answers to threads.  See MONO_BCL_TESTS_XUNIT_THREADS.
  bcl-xunit-System.Core/rest
  # 1854s with the machine to itself, and the most expensive test in the tree.
  # 1912s of CPU against that, so it is one core's worth of work end to end --
  # most of it in the C# compilers it forks, not in the console.
  bcl-System.Web/MonoTests.System.Web.UI.WebControls
  # 707s, of which TemplateControlCompilerTest is 555 -- the same page-compiling
  # cost, concentrated in one fixture.
  bcl-System.Web/MonoTests.System.Web.Compilation
  # 527s, PageTest the largest single fixture at 167s.
  bcl-System.Web/MonoTests.System.Web.UI
  # 332s, half of it DynamicControlTest.
  bcl-System.Web.DynamicData
  # 922s, the longest single test in the tree, and unsplittable: all 124 cases
  # live in one fixture.  Every case launches a debuggee and waits for it.
  bcl-Mono.Debugger.Soft
)

# Tests that want more than one core, as `<test name>=<cores>`.  CTest charges
# every test one slot of `-j` unless told otherwise, so a suite that really
# runs several threads oversubscribes the machine for as long as it runs.
#
# Almost nothing here needs an entry, including the expensive suites: a slow
# BCL test is normally slow because it forks a C# compiler and waits, so it
# holds about one core no matter how many threads it has open.
# MonoTests.System.Web.UI.WebControls opens dozens and still measured 1912s of
# CPU against 1854s of wall.  The exceptions are the suites that exist to
# exercise parallel machinery, where the threads are the point.
#
# Not that a name tells you which those are.  Every BCL test named for threads,
# tasks, concurrency or parallelism was measured -- thirteen of them -- and all
# but the two below came back within a few percent of one core.  The nunit
# Dataflow suite manages 0.52: it spends its time waiting on blocks rather than
# running them.
#
# The numbers below are CPU-seconds over wall-seconds for the whole process
# tree, each read twice under different machine load.  Reading twice is the
# point: contention can only push a ratio down, so a single number is just a
# lower bound, but one that does not move when the load around it does is the
# test's own appetite.  Rounded to nearest -- over-reserving wastes a slot as
# surely as under-reserving oversubscribes one.
set(MONO_BCL_TESTS_PROCESSORS
  # 1.62 and 1.63.  PLINQ partitions across the pool, so this is the one suite
  # here that clearly wants a second core.
  bcl-xunit-System.Core/System.Linq.Parallel.Tests=2
  # 1.49 and 1.53 -- close enough to the 1.5 boundary that either value would
  # be defensible.
  bcl-corlib/MonoTests.System.Threading.Tasks=2
)

# xunit suites that run their collections concurrently, as `<test name>=<threads>`.
# Everything not named here runs `-parallel none`, which is what the whole xunit
# half ran before this list existed.
#
# A name here does two things: the console gets
# `-parallel collections -maxthreads <threads>`, and the test reserves that many
# slots of CTest's `-j` pool.  They are deliberately the same number and
# deliberately not a second entry in MONO_BCL_TESTS_PROCESSORS -- a suite handed
# threads but not slots oversubscribes the machine for its whole run, and two
# lists would let the next person to add a suite update one and not the other
# without anything failing to tell them.
#
# Whether a suite can take this at all is a property of the assembly, not of the
# machine.  xunit's default is a collection per class, so an assembly with no
# [Collection] and no [assembly: CollectionBehavior] presents one schedulable
# unit per test class; nineteen corefx test directories carry one of those
# attributes and would run serially however many threads they were given.  Check
# before adding, because an inert entry here reads as "parallelism was tried and
# did not help".  Beyond that the cases have to tolerate sharing a process:
# `-noappdomain -noshadow` are already passed, so there is no appdomain
# isolation for parallel collections to fall back on and the assembly's statics
# are shared.
#
# The win is bounded by the console's own startup, which stays serial: ~41-49s
# of JIT before the first case runs.
set(MONO_BCL_TESTS_XUNIT_THREADS
  # The whole assembly bar PLINQ -- see MONO_BCL_TESTS_XUNIT_SERIAL for why it
  # is one group rather than seven.  Four rather than more because the reservation
  # is charged for the run's full length while the threads are only busy after
  # the console has started, and because a machine running this is normally also
  # running the rest of the sweep.
  bcl-xunit-System.Core/rest=4
)

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

set(MONO_TEST_FIXTURE_FIELDS
    PROFILE ASSEMBLY NAME SOURCES REFS FLAGS PROGRAM IN_TESTS_DIR)

# Declares a fixture assembly for one directory's suite.  Called from that
# directory's extra.cmake.
#
#   mono_test_fixture_assembly(PROFILE <p> ASSEMBLY <lib.dll> NAME <fixture.dll>
#                              SOURCES <file>... [REFS <assembly>...]
#                              [PROGRAM] [IN_TESTS_DIR] [FLAGS <flag>...])
#
# IN_TESTS_DIR puts it beside the test assemblies instead, for the suites that
# find it by looking next to themselves.
#
# Recorded here and built later, with the assemblies: REFS names a reference the
# way a library does, and the map from a bare name to the target that produces
# it is not filled in until every directory has been read.
function(mono_test_fixture_assembly)
  cmake_parse_arguments(F "PROGRAM;IN_TESTS_DIR" "PROFILE;ASSEMBLY;NAME"
                        "SOURCES;REFS;FLAGS" ${ARGN})
  # The sources are relative to the directory declaring them, which is not the
  # directory this is built from.
  set(_abs "")
  foreach(_s IN LISTS F_SOURCES)
    get_filename_component(_s "${_s}" ABSOLUTE)
    list(APPEND _abs "${_s}")
  endforeach()
  set(F_SOURCES "${_abs}")

  get_property(_n GLOBAL PROPERTY MONO_TEST_FIXTURE_COUNT)
  if(NOT _n)
    set(_n 0)
  endif()
  foreach(_f IN LISTS MONO_TEST_FIXTURE_FIELDS)
    set_property(GLOBAL PROPERTY MONO_TEST_FIXTURE_${_n}_${_f} "${F_${_f}}")
  endforeach()
  math(EXPR _n "${_n} + 1")
  set_property(GLOBAL PROPERTY MONO_TEST_FIXTURE_COUNT ${_n})

  # The suite that embeds this asks for the target by name while it is being
  # materialized, so the name is settled here rather than below.
  _mono_stem(_stem "${F_NAME}")
  _mono_stem(_astem "${F_ASSEMBLY}")
  set_property(GLOBAL APPEND PROPERTY MONO_TEST_FIXTURES_${F_PROFILE}_${_astem}
               "mcs-${F_PROFILE}-${_astem}-fixture-${_stem}")
endfunction()

# Turns every fixture declaration into a target.  Called once, from
# mono_managed_materialize(), after the provider map exists.
function(mono_test_fixtures_materialize)
  get_property(_count GLOBAL PROPERTY MONO_TEST_FIXTURE_COUNT)
  if(NOT _count)
    return()
  endif()
  math(EXPR _last "${_count} - 1")
  foreach(_i RANGE ${_last})
    _mono_materialize_test_fixture(${_i})
  endforeach()
endfunction()

function(_mono_materialize_test_fixture i)
  foreach(_f IN LISTS MONO_TEST_FIXTURE_FIELDS)
    get_property(F_${_f} GLOBAL PROPERTY MONO_TEST_FIXTURE_${i}_${_f})
  endforeach()
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
    if(NOT _p)
      message(FATAL_ERROR
              "fixture ${F_NAME}: nothing produces ${_r} in profile ${F_PROFILE}")
    endif()
    list(APPEND _deps "${_p}")
  endforeach()

  set(_srcs ${F_SOURCES})

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

# Skips named methods of one xunit suite, for tests that cannot pass against
# this runtime whatever the configuration.  Say why at the call site.
#
#   mono_test_xunit_exclude(PROFILE <p> ASSEMBLY <lib.dll> METHODS <full.name>...)
function(mono_test_xunit_exclude)
  cmake_parse_arguments(E "" "PROFILE;ASSEMBLY" "METHODS" ${ARGN})
  _mono_stem(_stem "${E_ASSEMBLY}")
  set_property(GLOBAL APPEND PROPERTY
               MONO_TEST_XUNIT_NOMETHOD_${E_PROFILE}_${_stem} ${E_METHODS})
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

# Lists the test classes in an xunit assembly.  One per profile, built on first
# use; the console in external/xunit-binaries has no listing mode of its own.
function(_mono_xunit_lister out profile)
  mono_profile_dir(_pdir ${profile})
  set(_exe "${_pdir}/tests/xunit-lister.exe")
  set(${out} "${_exe}" PARENT_SCOPE)
  if(TARGET mcs-${profile}-xunit-lister)
    return()
  endif()
  set(_src "${MONO_MCS_TOPDIR}/tools/xunit-lister/xunit-lister.cs")
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
    COMMENT "CSC [${profile}] xunit-lister.exe"
    VERBATIM)
  add_custom_target(mcs-${profile}-xunit-lister DEPENDS "${_exe}")
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
  # `_command` stops short of the result file, which the CTest side appends: a
  # split suite runs its groups in parallel and each needs a result of its own.
  set(_kind "${kind}")
  set(_workdir "${dir}")
  set(_testname "bcl-${_stem}")
  set(_deps "${_out}")
  set(_resultbase "${_testdir}/TestResult-${profile}-${_stem}")
  set(_listing "${MONO_MANAGED_DEPSDIR}/${_testtarget}.listing")
  if(kind STREQUAL nunit)
    _mono_test_runner(_runner "${_stem}" ${profile} "${dir}"
                      "${T_GLOBAL_CONFIG}" "${T_RUNTIME_CONFIG}"
                      "${T_RUNNER_FILES}")
    list(APPEND _deps "${_runner}")
    string(JOIN "," _excludes ${MONO_TEST_NUNIT_EXCLUDES} ${T_EXCLUDES})
    set(_command "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" --debug "${_runner}"
                 "${_out}" "-exclude=${_excludes}" -format:nunit2)
    set(_mono_path "${_pdir}:${_testdir}:${dir}")
    # -explore builds the whole tree and prints it instead of running it.
    set(_lister "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" --debug "${_runner}"
                "${_out}" -noheader "-explore:${_listing}")
    set(_listing_deps "${_runner}")
    # A group where nothing was left to run after -exclude is a skip, not a pass.
    set(_skiprx "Tests run: 0,")
  else()
    set(_testname "bcl-xunit-${_stem}")
    set(_resultbase "${_resultbase}-xunit")
    # No -parallel here: it is per-test rather than per-assembly, and the CTest
    # side appends it along with the reservation that has to match it.  See
    # MONO_BCL_TESTS_XUNIT_THREADS.
    set(_command "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" --debug
                 "${MONO_TEST_XUNIT_DIR}/xunit.console.exe" "${_out}"
                 -noappdomain -noshadow)
    foreach(_t IN LISTS MONO_TEST_XUNIT_NOTRAITS)
      list(APPEND _command -notrait "${_t}")
    endforeach()
    get_property(_nomethods GLOBAL PROPERTY
                 MONO_TEST_XUNIT_NOMETHOD_${profile}_${_astem})
    foreach(_m IN LISTS _nomethods)
      list(APPEND _command -nomethod "${_m}")
    endforeach()
    _mono_xunit_runtime(_xrt ${profile})
    list(APPEND _deps ${_xrt})
    set(_mono_path "${_pdir}:${_testdir}:${dir}:${MONO_TEST_XUNIT_DIR}")
    if(T_REMOTE_EXECUTOR)
      _mono_remote_executor(_remote ${profile})
      set(_env_extra "REMOTE_EXECUTOR=${_remote}")
    endif()
    _mono_xunit_lister(_xl ${profile})
    set(_lister "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" "${_xl}" "${_out}")
    set(_listing_deps "${_xl}")
    # xunit spells the assembly with its extension only in the summary of a run
    # that matched nothing, which is what makes this safe to look for in output
    # a test could otherwise have written itself.
    set(_skiprx "_xunit-test.dll  Total: 0")
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

  set(_timeout ${MONO_BCL_TEST_TIMEOUT})
  if(_testname IN_LIST MONO_BCL_TESTS_LONG)
    set(_timeout ${MONO_BCL_TEST_LONG_TIMEOUT})
  endif()

  # An integration suite against a broker on localhost: 61 of its 87 cases need
  # one, so there is no useful remainder to keep. See MONO_ENABLE_NETWORK_TESTS.
  if(_testname STREQUAL "bcl-System.Messaging" AND NOT MONO_ENABLE_NETWORK_TESTS)
    return()
  endif()

  # Winforms opens a display before it runs a case, so with none it dies in
  # libX11's IO error handler rather than failing a test. See
  # MONO_ENABLE_GUI_TESTS.
  if(_testname STREQUAL "bcl-System.Windows.Forms" AND NOT MONO_ENABLE_GUI_TESTS)
    return()
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
  set(_env "MONO_PATH=${_mono_path};MONO_REGISTRY_PATH=$ENV{HOME}/.mono/registry;MONO_TESTS_IN_PROGRESS=yes;PATH=${CMAKE_BINARY_DIR}/runtime/_tmpinst/bin:$ENV{PATH};LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/mono/profiler:$ENV{LD_LIBRARY_PATH};${_env_extra}${_env_dir}")

  _mono_bcl_register()
endfunction()

# Registers one suite with CTest, splitting it when it is in
# MONO_BCL_TESTS_SPLIT.
#
# The registration itself lives in a generated file named in TEST_INCLUDE_FILES,
# because what a suite becomes is not known until the assembly has been listed,
# and that cannot happen while CMake is still configuring.  The build writes the
# group list beside it; the generated file runs the assembly whole whenever that
# list is missing.
#
# A macro rather than a function: it reads and writes the variables its caller
# already has in hand -- _kind, _testname, _testtarget, _workdir, _command,
# _lister, _listing, _listing_deps, _out, _deps, _resultbase, _timeout, _label,
# _skiprx and _env -- and its template needs them under those names.  _timeout is
# the assembly's, and every test the template registers gets it.
macro(_mono_bcl_register)
  set(_groupfile "${MONO_MANAGED_DEPSDIR}/${_testtarget}.groups.cmake")
  set(_ctestfile "${MONO_MANAGED_DEPSDIR}/${_testtarget}.ctest.cmake")
  set(_max_groups ${MONO_BCL_SPLIT_MAX_GROUPS_${_kind}})
  set(_split 0)

  # The namespaces this suite has to keep out of its shared console.  A suite
  # with any is cut for isolation instead of by weight, so _max_groups no longer
  # applies to it -- see MonoBclDiscover.cmake.
  set(_quarantine "")
  foreach(_q IN LISTS MONO_BCL_TESTS_XUNIT_SERIAL)
    # Matched in its own command rather than inside the if(): CMAKE_MATCH_1 is
    # expanded when the if()'s arguments are, which is before its own MATCHES
    # would have set it.
    string(REGEX MATCH "^(.*)/([^/]+)$" _qm "${_q}")
    if(_qm AND "${CMAKE_MATCH_1}" STREQUAL "${_testname}")
      list(APPEND _quarantine "${CMAKE_MATCH_2}")
    endif()
  endforeach()

  if("${_testname}" IN_LIST MONO_BCL_TESTS_SPLIT)
    set(_split 1)
    set(_discover "${MONO_MANAGED_DEPSDIR}/${_testtarget}.discover.cmake")
    # The listing runs on the runtime being built, which _mono_tool_depends does
    # not cover when the tools are hosted on the system mono.  Without this the
    # listing races the link and reads a half-written mono-sgen.
    _mono_runtime_depends(_rtdeps)
    file(CONFIGURE OUTPUT "${_discover}" CONTENT [[
# Generated by MonoManagedTests.cmake -- do not edit.
set(BCL_KIND           [==[@_kind@]==])
set(BCL_TESTNAME       [==[@_testname@]==])
set(BCL_LISTER_COMMAND [==[@_lister@]==])
set(BCL_LISTING        [==[@_listing@]==])
set(BCL_WORKDIR        [==[@_workdir@]==])
set(BCL_ENVIRONMENT    [==[@_env@]==])
set(BCL_MAX_GROUPS     [==[@_max_groups@]==])
set(BCL_QUARANTINE     [==[@_quarantine@]==])
set(BCL_OUTPUT         [==[@_groupfile@]==])
]] @ONLY)
    add_custom_command(
      OUTPUT "${_groupfile}"
      COMMAND "${CMAKE_COMMAND}" -D "SETTINGS=${_discover}"
              -P "${CMAKE_SOURCE_DIR}/cmake/MonoBclDiscover.cmake"
      DEPENDS "${_out}" ${_deps} ${_listing_deps} ${_rtdeps} "${_discover}"
      COMMENT "LIST ${_testname}"
      VERBATIM)
    add_custom_target(${_testtarget}-groups DEPENDS "${_groupfile}")
    add_dependencies(${_testtarget} ${_testtarget}-groups)
  endif()

  if(_quarantine AND NOT (_split AND "${_kind}" STREQUAL xunit))
    message(WARNING "MONO_BCL_TESTS_XUNIT_SERIAL names ${_testname}, which is not a split xunit suite")
  endif()

  configure_file("${CMAKE_SOURCE_DIR}/cmake/MonoBclTests.cmake.in"
                 "${_ctestfile}" @ONLY)
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_ctestfile}")
endmacro()

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
