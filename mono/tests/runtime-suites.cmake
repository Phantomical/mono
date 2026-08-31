# CTest wiring for the runtime corpus.
#
# Every program in the corpus is its own CTest test, named `<suite>/<program>`
# -- so `ctest -R runtime/bug-18026` runs exactly one of them, `--rerun-failed`
# re-runs only what broke, and a suite that covers the same programs several
# ways (the four gshared optimization sets, the thirty-odd SGen collector
# configurations) reports which configuration failed rather than just that one
# did. Each also runs on every collector that was built, `...@sgen` and
# `...@boehm`. The corresponding cost is that `ctest -N` lists ~3400 tests.
#
# Labels. `ctest` with no arguments runs the fast set. Everything heavy is
# behind a label so the inner loop stays short. See the `check` target.
#   runtime   the ~700-program corpus and the one-off suites
#   gshared   generic sharing, over four optimization sets
#   sgen      the SGen collector matrix
#   interp    the whole corpus again under the interpreter
#   slow      minutes-long single tests
#   stress    long-running stress tests

set(_class_dir "${_class}")
set(_run_test "${CMAKE_SOURCE_DIR}/cmake/MonoRunTest.cmake")

# Where the per-test temporary directories go. Under /tmp rather than the build
# tree so that TMPDIR still means what the corpus expects of it, and keyed by
# the build directory so two worktrees running the suite at once get their own.
string(SHA256 _tmp_key "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${_tmp_key}" 0 12 _tmp_key)
set(_tmp_root "/tmp/mono-tests-${_tmp_key}")

# Which collector each suite runs on. The corpus is collector-agnostic, so it
# runs on every runtime that was built -- mono-wrapper picks the binary out of
# MONO_EXECUTABLE, so this costs an environment variable and a name suffix.
#
# The SGen matrix is the exception and asks for `GC sgen`. mono-boehm accepts
# --gc=sgen and --gc-params, ignores them, and exits 0, so running those suites
# on it would add a hundred and sixty passing tests that assert nothing.
set(_mono_gcs "")
if(MONO_ENABLE_SGEN)
  list(APPEND _mono_gcs sgen)
endif()
if(MONO_ENABLE_BOEHM)
  list(APPEND _mono_gcs boehm)
endif()

# The collector a test ran on lives in its name (`...@sgen`, `...@boehm`) and
# not in a label. `ctest -L` matches labels as a regex, so a `gc-sgen` label
# would also be picked up by `-L sgen` -- which already means the collector
# matrix, a different set. Select a collector with `-R '@boehm$'`, and combine
# it with `-L` as usual.
function(_mono_gc_env out_env gc)
  set(${out_env} "MONO_EXECUTABLE=${CMAKE_BINARY_DIR}/mono/mini/mono-${gc}" PARENT_SCOPE)
endfunction()

# Tests that saturate the machine on their own -- they scale their own thread
# count off Environment.ProcessorCount, or hammer the thread pool. CTest assumes
# one core per test, packs `-j` of them alongside everything else, and starves
# them: appdomain-threadpool-unload runs in 3s and has twice timed out at 300s
# in a full `check-all`. Claiming several cores each keeps that from happening.
set(_mono_parallel_hungry
  appdomain-threadpool-unload.exe
  process-unref-race.exe
  namedmutex-destroy-race.exe
  pinvoke-detach-1.exe
  bug-18026.exe
)

# mono_runtime_suite(<name> TESTS ... [LABEL x] [RUNTIME_ARGS s] [ENV ...]
#                    [OPT_SETS s] [TIMEOUT n] [EXPECT n] [WORKDIR d]
#                    [PROCESSORS n] [GC ...] [SKIP_BOEHM ...]
#                    [LONG ... [LONG_TIMEOUT n]])
#
# One CTest test per program, named `<suite>/<program>` -- and per optimization
# set on top of that, `<suite>/<program>:<opt-set>`, since those are separate
# runs that fail separately.
#
# This is deliberately not test-runner.exe driving a whole list. CTest's own
# scheduler then owns the parallelism, `ctest -R` addresses one program, and
# `--rerun-failed` re-runs the six that broke rather than all seven hundred.
# What test-runner did per child is small enough to reproduce inline: set
# MONO_PATH/MONO_CONFIG, hand the runtime `-O=<opt-set>` and the suite's
# runtime arguments, and compare the exit code. TEST_DRIVER_TIMEOUT_SEC still
# carries the per-test timeout, as it did on each child test-runner spawned.
function(mono_runtime_suite name)
  cmake_parse_arguments(ARG "" "LABEL;RUNTIME_ARGS;OPT_SETS;TIMEOUT;EXPECT;WORKDIR;PROCESSORS;LONG_TIMEOUT"
                            "TESTS;ENV;GC;SKIP_BOEHM;LONG" ${ARGN})
  if(NOT ARG_TESTS)
    return()
  endif()
  if(NOT ARG_GC)
    set(ARG_GC ${_mono_gcs})
  else()
    # A suite can ask for a collector this build did not produce. Drop it
    # rather than emitting a test that cannot run.
    set(_want "")
    foreach(_g IN LISTS ARG_GC)
      if(_g IN_LIST _mono_gcs)
        list(APPEND _want "${_g}")
      endif()
    endforeach()
    set(ARG_GC "${_want}")
  endif()
  if(NOT ARG_GC)
    return()
  endif()
  if(NOT ARG_LABEL)
    set(ARG_LABEL runtime)
  endif()
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 300)
  endif()
  if(NOT ARG_LONG_TIMEOUT)
    set(ARG_LONG_TIMEOUT 900)
  endif()
  if(NOT ARG_EXPECT)
    set(ARG_EXPECT 0)
  endif()
  if(NOT ARG_WORKDIR)
    set(ARG_WORKDIR "${_bin}")
  endif()

  # Both arrive as one space-separated string, the shape the automake recipes
  # used and the shape test-runner.exe parsed.
  set(_rt_args "")
  if(ARG_RUNTIME_ARGS)
    separate_arguments(_rt_args UNIX_COMMAND "${ARG_RUNTIME_ARGS}")
  endif()
  set(_opt_sets "")
  if(ARG_OPT_SETS)
    separate_arguments(_opt_sets UNIX_COMMAND "${ARG_OPT_SETS}")
  endif()
  # `-` stands for "no optimization set", so the loop below still runs once for
  # a suite that asked for none. An empty element cannot say that: CMake does
  # not distinguish a one-empty-element list from an empty one, and an empty
  # list iterates zero times.
  if(NOT _opt_sets)
    set(_opt_sets "-")
  endif()

  foreach(_gc IN LISTS ARG_GC)
    _mono_gc_env(_gc_env "${_gc}")
    # SKIP_BOEHM drops the tests that fail on Boehm from the boehm half only,
    # so the sgen half of the suite still covers them.
    set(_gc_tests ${ARG_TESTS})
    if(_gc STREQUAL "boehm" AND ARG_SKIP_BOEHM)
      list(REMOVE_ITEM _gc_tests ${ARG_SKIP_BOEHM})
    endif()

    foreach(_test IN LISTS _gc_tests)
      string(REGEX REPLACE "\\.exe$" "" _stem "${_test}")

      # LONG names the programs whose work does not fit the suite's budget --
      # not slow by accident, but asking for far more of the runtime than their
      # neighbours do.
      set(_timeout ${ARG_TIMEOUT})
      if(_test IN_LIST ARG_LONG)
        set(_timeout ${ARG_LONG_TIMEOUT})
      endif()
      # A little above what MonoRunTest gives the test, so the SIGQUIT thread
      # dump wins the race and CTest only steps in if that failed too.
      math(EXPR _ctest_timeout "${_timeout} + 60")

      foreach(_opt IN LISTS _opt_sets)
        if(_opt STREQUAL "-")
          set(_tname "${name}/${_stem}")
          set(_oarg "")
        else()
          set(_tname "${name}/${_stem}:${_opt}")
          set(_oarg "-O=${_opt}")
        endif()
        set(_gname "${_tname}@${_gc}")
        string(REGEX REPLACE "[^A-Za-z0-9._-]" "_" _tmpdir "${_gname}")
        set(_tmpdir "${_tmp_root}/${_tmpdir}")
        add_test(NAME "${_gname}"
                 COMMAND "${CMAKE_COMMAND}" -E env
                         "MONO_PATH=${_class_dir}"
                         "MONO_CONFIG=${_bin}/tests-config"
                         "TEST_DRIVER_TIMEOUT_SEC=${_timeout}"
                         "${_gc_env}"
                         ${ARG_ENV}
                         "${CMAKE_COMMAND}"
                         "-DMONO_TEST_EXPECT=${ARG_EXPECT}"
                         "-DMONO_TEST_TIMEOUT=${_timeout}"
                         "-DMONO_TIMEOUT_BINARY=${MONO_TIMEOUT_BINARY}"
                         "-DMONO_TEST_TMPDIR=${_tmpdir}"
                         -P "${_run_test}" --
                         "${_wrapper}" ${_oarg} ${_rt_args} "${_test}"
                 WORKING_DIRECTORY "${ARG_WORKDIR}")
        set_tests_properties("${_gname}" PROPERTIES
          LABELS "${ARG_LABEL}"
          TIMEOUT ${_ctest_timeout})
        if(ARG_PROCESSORS)
          set_tests_properties("${_gname}" PROPERTIES PROCESSORS ${ARG_PROCESSORS})
        elseif(_test IN_LIST _mono_parallel_hungry)
          set_tests_properties("${_gname}" PROPERTIES PROCESSORS 4)
        endif()
      endforeach()
    endforeach()
  endforeach()
endfunction()

# Turn a source list into the .exe list a suite takes, minus the disabled.
function(_mono_exe_list out)
  set(_r "")
  foreach(_s IN LISTS ARGN)
    string(REGEX REPLACE "\\.(cs|il)$" ".exe" _e "${_s}")
    if(NOT _e IN_LIST MONO_TESTS_DISABLED)
      list(APPEND _r "${_e}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _r)
  set(${out} "${_r}" PARENT_SCOPE)
endfunction()

_mono_exe_list(_regular ${MONO_TESTS_CS_SRC} ${MONO_TESTS_IL_SRC} ${MONO_TESTS_BENCH_SRC})
_mono_exe_list(_gshared ${MONO_TESTS_GSHARED_SRC})
_mono_exe_list(_stress  ${MONO_TESTS_STRESS_SRC})
_mono_exe_list(_stress_process ${MONO_TESTS_STRESS_PROCESS_SRC})
_mono_exe_list(_sgen_regular   ${MONO_TESTS_SGEN_REGULAR_SRC})
_mono_exe_list(_sgen_toggleref ${MONO_TESTS_SGEN_TOGGLEREF_SRC})
_mono_exe_list(_sgen_bridge    ${MONO_TESTS_SGEN_BRIDGE_SRC})
_mono_exe_list(_sgen_bridge2   ${MONO_TESTS_SGEN_BRIDGE2_SRC})
_mono_exe_list(_sgen_bridge3   ${MONO_TESTS_SGEN_BRIDGE3_SRC})

# Tailcall: the compile list minus the ones that build but do not pass.
_mono_exe_list(_tailcall_all ${MONO_TESTS_TAILCALL_CS_SRC} ${MONO_TESTS_TAILCALL_IL_SRC})
set(_tailcall ${_tailcall_all})
list(REMOVE_ITEM _tailcall
     ${MONO_TESTS_TAILCALL_DISABLED_COMPILE} ${MONO_TESTS_TAILCALL_DISABLED_RUN})

# The suites
#
# The corpus default is 300s, which the three below have been measured getting
# uncomfortably close to on a machine with its cores busy. Each is slow for a
# reason its neighbours are not, so each gets the long budget rather than the
# whole corpus being loosened to cover them.
#
#   dynamic-method-churn  asks the JIT for 40000 compiles -- 20000 dynamic
#     methods, each with a runtime-invoke wrapper of its own, since a dynamic
#     method cannot share the cached one. That is minutes of LLVM at any
#     per-method cost this backend could plausibly reach. Measured at 412s.
#   appdomain-threadpool-unload  unloads 100 domains from a PLINQ query sized
#     to ProcessorCount, each with a thread-pool item spinning in it. It wants
#     the whole machine and gets a share of it, so its cost is set by what else
#     is running: 140s, 227s and 266s across runs, and killed at the 300s mark
#     on the boehm half of a full sweep.
#   appdomain-unload  creates and unloads domains with deliberately slow
#     finalizers and a 10s BeginInvoke still in flight, so most of its time is
#     spent waiting rather than running -- 171s to 247s measured, which is not
#     margin enough to leave at 300s.
#
# Tier 0 is off, so every method here is compiled. Most of these programs run
# their body once, which is too few calls to spend a counter, so at the default
# tier they would test the interpreter instead of the backend -- the tier-0 arm
# below is where that configuration is covered.
mono_runtime_suite(runtime TESTS ${_regular}
                   ENV "MONO_LLVM_JIT_TIER0=0"
                   SKIP_BOEHM ${MONO_TESTS_BOEHM_DISABLED}
                   LONG dynamic-method-churn.exe
                        appdomain-unload.exe
                        appdomain-threadpool-unload.exe
                        bug-18026.exe)

# CoreCLR's tailcall corpus, run like any other program: several of these
# recurse deeply enough that a missed tail call is a stack overflow rather than
# a subtle difference, so running them is the check.
mono_runtime_suite(runtime-tailcall TESTS ${_tailcall}
                   ENV "MONO_LLVM_JIT_TIER0=0")

mono_runtime_suite(gshared LABEL gshared TESTS ${_gshared})

# Every allocation is kept while sequence points are on, because a debugger stops
# at one and can hand any object a frame holds to a method it is asked to call.
# allocation_is_observable () answers yes for every class there, so this arm is
# the one that reaches mono.alloc.object.kept for a class nothing else marks, and
# the only one that reaches mono.alloc.vector.kept at all.
mono_runtime_suite(runtime-alloc-kept TESTS alloc-elide.exe
                   ENV "MONO_LLVM_JIT_TIER0=0" "MONO_DEBUG=gen-seq-points")

# GVN and DSE read allockind(zeroed), and tier 1 runs neither. The threshold is
# low enough that each arm promotes inside the loop in Main.
mono_runtime_suite(runtime-alloc-zeroed TESTS alloc-elide.exe
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=100000")

# The copy a value type with references moves as, and the cards behind it.
# check-remset-consistency walks the old heap at each minor collection and
# aborts on an old-to-young reference no remembered set holds, which is what a
# missing card leaves behind. SGen only -- Boehm parses MONO_GC_DEBUG itself and
# knows only do-not-finalize and log-finalizers, so it would run this arm with
# no check at all. The threshold reaches tier 2, where the copy meets SROA and
# the dead-allocation walk. The arms are short enough that the program ends
# first at the default.
mono_runtime_suite(runtime-value-copy TESTS value-copy.exe GC sgen
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=100000"
                       "MONO_GC_DEBUG=check-remset-consistency")

# Continuations, whose two outcomes want naming rather than accepting either.
# With everything compiled and a collector that keeps the saved stack out of the
# heap, they work. Boehm is the collector that does not, and the tier-0 arm
# further down is the engine that does not.
mono_runtime_suite(runtime-tasklets TESTS tasklets.exe GC sgen
                   ENV "MONO_LLVM_JIT_TIER0=0" "MONO_TEST_TASKLETS=run")
mono_runtime_suite(runtime-tasklets-boehm TESTS tasklets.exe GC boehm
                   ENV "MONO_LLVM_JIT_TIER0=0" "MONO_TEST_TASKLETS=refuse")

if(MONO_ENABLE_INTERPRETER)
  set(_interp ${_regular})
  list(REMOVE_ITEM _interp ${MONO_TESTS_INTERP_DISABLED})
  # The same ~700 programs again on a much slower engine. Useful, but not on
  # every edit, so it gets a label of its own rather than sitting in `runtime`.
  # Only appdomain-threadpool-unload needs the long budget here. The other two
  # the JIT gives it to are cheap under the interpreter -- their cost is
  # compilation, and there is none -- but this one's cost is domain unloads
  # against a saturated machine, which the interpreter does not make faster.
  mono_runtime_suite(runtime-interp LABEL interp TESTS ${_interp}
                     RUNTIME_ARGS "--interpreter"
                     SKIP_BOEHM ${MONO_TESTS_BOEHM_DISABLED}
                     LONG appdomain-threadpool-unload.exe
                          bug-18026.exe)

  # The whole corpus at the default tier, which is neither of the two above:
  # each method starts interpreted and the hot ones are compiled underneath it,
  # so the two engines are in one process and a method can change engine while
  # its callers are running.
  #
  # Built from the full corpus rather than from the interpreter's set, because a
  # program the pure interpreter cannot run may be perfectly fine once its hot
  # methods compile. This is the tier real users get, so it is the one that
  # can least afford to inherit somebody else's refusals. Anything that genuinely
  # cannot run here belongs in MONO_TESTS_TIER0_DISABLED, with the reason.
  set(_tier0 ${_regular})
  list(REMOVE_ITEM _tier0 ${MONO_TESTS_TIER0_DISABLED})
  mono_runtime_suite(runtime-tier0 LABEL interp TESTS ${_tier0}
                     SKIP_BOEHM ${MONO_TESTS_BOEHM_DISABLED}
                     LONG appdomain-threadpool-unload.exe
                          dynamic-method-churn.exe
                          appdomain-unload.exe
                          bug-18026.exe)

  # The tailcall corpus at the default tier, where a tail site is the
  # interpreter's rather than the backend's. Both engines have to honour the
  # same shapes, and a program that alternates between them has to hop without
  # growing the stack.
  mono_runtime_suite(runtime-tailcall-tier0 LABEL interp TESTS ${_tailcall})

  # Both engines in one process, which neither of the suites above covers:
  # Callee's methods interpret while its caller compiles, so every call in it
  # crosses the boundary. Argument placement there is settled by the compiler
  # rather than fixed by an ABI document, and being wrong about it corrupts
  # values rather than crashing - so it wants a test that reads them back.
  mono_runtime_suite(runtime-interp-entries LABEL interp
                     TESTS interp-entries.exe
                     ENV "MONO_LLVM_JIT_TIER0=Callee")

  # The other direction: Interpreted's methods run in the interpreter while
  # their callees compile, so every call in Run () leaves the interpreter for
  # native code. No suite above covers that crossing -- they either compile
  # everything or interpret everything, and by default every callee is
  # interpreted too.
  mono_runtime_suite(runtime-interp-calls-compiled LABEL interp
                     TESTS interp-calls-compiled.exe
                     ENV "MONO_LLVM_JIT_TIER0=Interpreted")

  # The same crossing into a wrapper. A dynamic method is the shape it is
  # written for: tier 0 accepts that kind, so a compiled one used to be entered
  # by interpreting its bytecode whenever an interpreted frame called it.
  # Pinning tier 0 to Interpreted is what keeps Run () in the interpreter while
  # its callees compile.
  mono_runtime_suite(runtime-interp-jit-call-wrappers LABEL interp
                     TESTS interp-jit-call-wrappers.exe
                     ENV "MONO_LLVM_JIT_TIER0=Interpreted")

  # And at the default tier, where the wrappers the test calls start in the
  # interpreter instead of compiling on the thread that needed them.
  mono_runtime_suite(runtime-tier0-jit-call-wrappers LABEL interp
                     TESTS interp-jit-call-wrappers.exe)

  # Continuations at the default tier, where the frame that marked one is
  # interpreted and so has no native stack for Store () to copy. Mark () has to
  # say so rather than walk a stack it cannot describe.
  mono_runtime_suite(runtime-tasklets-tier0 LABEL interp TESTS tasklets.exe
                     ENV "MONO_TEST_TASKLETS=refuse")

  # `--interp=jit=<class>` compiles the named class and interprets the rest,
  # which is the only way today to get a compiled frame and an interpreted one
  # into the same process. Values the two engines hand each other -- a method
  # pointer above all -- only have to agree here.
  mono_runtime_suite(runtime-interp-jit LABEL interp
                     TESTS interp-jit-delegate.exe
                     RUNTIME_ARGS "--interpreter --interp=jit=Creator")
endif()

# domain-stress runs the appdomain create/unload loop for a fixed iteration
# count rather than a fixed duration, so its wall time is whatever the machine
# gives it. A full run has been measured at 816s of the suite's 900s, and it has
# been killed at the 900s mark on both collectors -- which reports as a plain
# failure, not a timeout, because it is the driver that does the killing.
mono_runtime_suite(runtime-stress LABEL stress TESTS ${_stress} TIMEOUT 900
                   LONG domain-stress.exe LONG_TIMEOUT 1800)
mono_runtime_suite(runtime-process-stress LABEL stress TESTS ${_stress_process} TIMEOUT 900)

# The SGen matrix. Each collector configuration is its own test, so a failure
# names the mode rather than just "sgen". The argument strings are verbatim
# from the automake recipes: the collector is selected on the command line and
# not through MONO_GC_PARAMS, and the toggleref and bridge suites need their
# test hooks (`toggleref-test`, `--gc-debug=bridge=...`) switched on or the
# behaviour they check never happens.
#
# PROCESSORS: these are GC stress programs, and most of the configurations run a
# concurrent or parallel collector, so one of them is worth several ordinary
# tests to the scheduler. Without the hint CTest packs `-j` of them alongside
# everything else and starves the rest -- `check-all` runs this label next to
# `interp`, where the symptom was an interpreted test that takes 7s timing out
# at 300s.
function(_mono_sgen_suite name tests args)
  mono_runtime_suite(${name} LABEL sgen GC sgen TESTS ${${tests}}
                     RUNTIME_ARGS "${args}" TIMEOUT 900 PROCESSORS 4)
endfunction()

_mono_sgen_suite(sgen-regular-ms-simple _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep,minor=simple")
_mono_sgen_suite(sgen-regular-ms-conc-simple _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc,minor=simple")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc-par,minor=simple")
_mono_sgen_suite(sgen-regular-ms-conc-split _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc,minor=split")
_mono_sgen_suite(sgen-regular-ms-conc-split-95-clear-at-gc _sgen_regular
                 "--gc=sgen --gc-debug=clear-at-gc --gc-params=major=marksweep-conc,minor=split,alloc-ratio=95")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple-par-dyn _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple-par-512k _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=512k")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple-par-32m _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=32m")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple-par-dyn-clear-at-gc _sgen_regular
                 "--gc=sgen --gc-debug=clear-at-gc --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery")
_mono_sgen_suite(sgen-toggleref-ms-simple _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep,minor=simple")
_mono_sgen_suite(sgen-toggleref-ms-conc-simple _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc,minor=simple")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple")
_mono_sgen_suite(sgen-toggleref-ms-conc-split _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc,minor=split")
_mono_sgen_suite(sgen-toggleref-ms-conc-split-95-clear-at-gc _sgen_toggleref
                 "--gc=sgen --gc-debug=clear-at-gc --gc-params=toggleref-test,major=marksweep-conc,minor=split,alloc-ratio=95")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple-par-dyn _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple-par,dynamic-nursery")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple-par-512k _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple-par,nursery-size=512k")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple-par-32m _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple-par,nursery-size=32m")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple-par-dyn-clear-at-gc _sgen_toggleref
                 "--gc=sgen --gc-debug=clear-at-gc --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple-par,dynamic-nursery")
_mono_sgen_suite(sgen-bridge-ms-simple-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-simple-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-split-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc,minor=split,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-simple-new-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=new")
_mono_sgen_suite(sgen-bridge-ms-conc-simple-old-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=old")
_mono_sgen_suite(sgen-bridge-ms-conc-par-simple-par-dyn-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-par-simple-par-512k-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=512k,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-par-simple-par-32m-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=32m,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-simple-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-simple-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-split-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc,minor=split,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-simple-new-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=new")
_mono_sgen_suite(sgen-bridge2-ms-conc-simple-old-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=old")
_mono_sgen_suite(sgen-bridge2-ms-conc-par-simple-par-dyn-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-par-simple-par-512k-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=512k,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-par-simple-par-32m-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=32m,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-simple-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-simple-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-split-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc,minor=split,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-simple-new-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=new")
_mono_sgen_suite(sgen-bridge3-ms-conc-simple-old-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=old")
_mono_sgen_suite(sgen-bridge3-ms-conc-par-simple-par-dyn-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-par-simple-par-512k-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=512k,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-par-simple-par-32m-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=32m,bridge-implementation=tarjan")

# A store from one old object to another marks its card only while a concurrent
# collection runs. The code that decides has no other exerciser.
#
# Under mod-union-consistency-check the collector reports a card the store
# missed, at the next collection and by the object it belongs to. Without the
# option the same miss is a payload the collection frees, and the program then
# reads a wrong value on the runs where the timing lands that way.
#
# One program rather than the whole list, because the check walks the heap at
# every collection.
mono_runtime_suite(sgen-wbarrier-mod-union LABEL sgen GC sgen TESTS sgen-wbarrier.exe
                   RUNTIME_ARGS "--gc=sgen --gc-debug=mod-union-consistency-check --gc-params=major=marksweep-conc,minor=simple"
                   TIMEOUT 900 PROCESSORS 4)

# One-off suites. These are not test-runner corpora: each is a single program
# whose exit code or output is the result.
# NATIVE is for the ones that do not run managed code on the runtime being
# built -- there is no collector to vary, so they get no @<gc> suffix.
function(mono_runtime_check name)
  cmake_parse_arguments(ARG "NATIVE" "TIMEOUT" "COMMAND;ENV;DEPENDS" ${ARGN})
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 300)
  endif()
  if(ARG_NATIVE)
    set(_gcs "")
  else()
    set(_gcs ${_mono_gcs})
  endif()

  foreach(_gc IN LISTS _gcs)
    _mono_gc_env(_gc_env "${_gc}")
    add_test(NAME "${name}@${_gc}"
             COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                     "${_gc_env}" ${ARG_ENV} ${ARG_COMMAND}
             WORKING_DIRECTORY "${_bin}")
    set_tests_properties("${name}@${_gc}" PROPERTIES
      LABELS runtime TIMEOUT ${ARG_TIMEOUT})
  endforeach()

  if(ARG_NATIVE)
    add_test(NAME ${name}
             COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                     ${ARG_ENV} ${ARG_COMMAND}
             WORKING_DIRECTORY "${_bin}")
    set_tests_properties(${name} PROPERTIES
      LABELS runtime TIMEOUT ${ARG_TIMEOUT})
  endif()
endfunction()

mono_runtime_check(runtime-type-load
  COMMAND "${_wrapper}" load-exceptions.exe)
mono_runtime_check(runtime-multi-netmodule
  COMMAND "${_wrapper}" test-multi-netmodule-4-exe.exe)
mono_runtime_check(runtime-cattr-type-load
  COMMAND "${_wrapper}" custom-attr-errors.exe)
mono_runtime_check(runtime-reflection-load-with-context
  COMMAND "${_wrapper}" reflection-load-with-context.exe)
mono_runtime_check(runtime-iomap-regression
  COMMAND "${_wrapper}" exists.exe ENV "MONO_IOMAP=all")
# The four unhandled-exception suites `check-local` ran: the exit code the
# runtime produces for an unhandled exception, with and without a managed
# AppDomain.UnhandledException handler installed.
#
# (There is also an `unhandled-exception-test-runner.2.exe` driver in the tree.
# automake had it behind a `test-unhandled-exception` target that `check` never
# invoked, and it currently reports a failing configuration, so it is not wired
# up here either.)
_mono_exe_list(_unhandled_1   ${MONO_TESTS_UNHANDLED_EXCEPTION_1_SRC})
_mono_exe_list(_unhandled_255 ${MONO_TESTS_UNHANDLED_EXCEPTION_255_SRC})

# TEST_UNHANDLED_EXCEPTION_HANDLER makes the test subscribe to
# AppDomain.UnhandledException, so setting it is the *with*-handler case.
# automake had these two target names the other way round. The names here
# follow the source.
function(_mono_unhandled_suite code handler)
  if(handler)
    set(_name "runtime-unhandled-exception-${code}-with-managed-handler")
    mono_runtime_suite(${_name} TESTS ${ARGN} EXPECT ${code}
                       ENV "TEST_UNHANDLED_EXCEPTION_HANDLER=1")
  else()
    set(_name "runtime-unhandled-exception-${code}-without-managed-handler")
    mono_runtime_suite(${_name} TESTS ${ARGN} EXPECT ${code})
  endif()
endfunction()

_mono_unhandled_suite(1   OFF ${_unhandled_1})
_mono_unhandled_suite(1   ON  ${_unhandled_1})
_mono_unhandled_suite(255 OFF ${_unhandled_255})
_mono_unhandled_suite(255 ON  ${_unhandled_255})

# A thread that leaves through pthread_exit drags glibc's forced unwind over
# every JIT'd frame below it. 42 is the exit code the program asks for once it
# has come back out of that.
_mono_exe_list(_forced_unwind ${MONO_TESTS_FORCED_UNWIND_SRC})
mono_runtime_suite(runtime-forced-unwind TESTS ${_forced_unwind} EXPECT 42)

# Shapes no threshold produces. One is an exception caught in a compiled frame
# with two interpreted frames under it, and a compiled one between them. The
# other is an interpreted frame calling through an address a compiled frame took.
# Promotion would eventually compile every method in either, so the ones that are
# to stay interpreted are named instead -- MONO_LLVM_JIT_TIER0 takes a substring
# of the printed name.
if(MONO_ENABLE_INTERPRETER)
  _mono_exe_list(_tier_pinned ${MONO_TESTS_TIER_PINNED_SRC}
                              ${MONO_TESTS_TIER_PINNED_IL_SRC})
  mono_runtime_suite(runtime-tier-pinned LABEL interp TESTS ${_tier_pinned}
                     ENV "MONO_LLVM_JIT_TIER0=InterpMe")
endif()

# The tier-2 cost model. Its root has to gather counts at tier 1 and then be
# compiled at tier 2 once, on the thread that asks - so self-promotion is turned
# off and the test drives the compile itself.
_mono_exe_list(_tier2_costed ${MONO_TESTS_TIER2_COSTED_SRC})
mono_runtime_suite(runtime-tier2-costed TESTS ${_tier2_costed}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0")

# The work half of the tier-2 counter, which promotes a body the calls alone
# never reach. The threshold is named here rather than left at its default, so
# the kernel spends it in the calls the test makes instead of in a run long
# enough to measure. The test carries its own control, a second kernel that takes
# the same calls and does far less work, so both arms assert that one. The second
# arm turns automatic promotion off and asserts that neither kernel moves.
_mono_exe_list(_tier2_cost_trigger ${MONO_TESTS_TIER2_COST_TRIGGER_SRC})
mono_runtime_suite(runtime-tier2-cost-trigger TESTS ${_tier2_cost_trigger}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=10000000")
mono_runtime_suite(runtime-tier2-cost-trigger-off TESTS ${_tier2_cost_trigger}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0")

# Entering a delegate's target directly. Two arms, on and off, the way
# runtime-tier2-cost-trigger has it. The root drives its own compiles, so
# self-promotion is off: what the test reads is the tier it asked for.
_mono_exe_list(_delegate_fold ${MONO_TESTS_DELEGATE_FOLD_SRC})
mono_runtime_suite(runtime-delegate-fold TESTS ${_delegate_fold}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0")
mono_runtime_suite(runtime-delegate-fold-off TESTS ${_delegate_fold}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_FOLD_DELEGATES=0")

# The guard a dispatch on an array receiver goes through. It is tier 2's, and
# the default threshold is far past what this program runs, so the arm that
# reaches it names one of its own. The off arm leaves every such site
# dispatching, which is the answer the guard has to agree with.
mono_runtime_suite(runtime-array-guard TESTS array-devirt.exe
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=10000")
mono_runtime_suite(runtime-array-guard-off TESTS array-devirt.exe
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=10000"
                       "MONO_LLVM_JIT_GUARD_ARRAYS=0")

# The guess GuardDispatchPass takes on a receiver it cannot prove a class
# for, behind the same array rule's guard. Tier 2 only, same as the array
# arm above, and the same threshold: this file spends the same shape of
# calls array-devirt.cs does to reach it. The off arm leaves every such site
# dispatching, which is the answer the guess has to agree with.
mono_runtime_suite(runtime-class-guard TESTS class-devirt.exe
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=10000")
mono_runtime_suite(runtime-class-guard-off TESTS class-devirt.exe
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=10000"
                       "MONO_LLVM_JIT_GUARD_CLASSES=0")

# The bonuses mono adds to the cost model. Two arms, on and off, the way
# runtime-tier2-cost-trigger has it. The root drives its own compiles, so
# self-promotion is off the way the costed suite has it, and the trivial
# pre-pass is off so that every fold the test reads is the cost model's.
# The threshold and the calibration behind it are the test file's subject.
_mono_exe_list(_tier2_inline_policy ${MONO_TESTS_TIER2_INLINE_POLICY_SRC})
mono_runtime_suite(runtime-tier2-inline-policy TESTS ${_tier2_inline_policy}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=110")
mono_runtime_suite(runtime-tier2-inline-policy-off TESTS ${_tier2_inline_policy}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=110 --llvm-opt=-mono-inline-devirt-return-bonus=0 --llvm-opt=-mono-inline-devirt-arg-bonus=0 --llvm-opt=-mono-inline-scalarize-arg-bonus=0 --llvm-opt=-mono-inline-dispatch-is-a-load=false --llvm-opt=-mono-inline-fold-vtable-fields=false")

# Whether the cost model folds a clause-bearing callee once its clause is dead.
# PromoteNow drives the compiles, so self-promotion is off, and the trivial
# pre-pass is off so a fold the test reads is never that one's instead.
_mono_exe_list(_tier2_inline_clause ${MONO_TESTS_TIER2_INLINE_CLAUSE_SRC})
mono_runtime_suite(runtime-tier2-inline-clause TESTS ${_tier2_inline_clause}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0")
mono_runtime_suite(runtime-tier2-inline-clause-off TESTS ${_tier2_inline_clause}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_LLVM_JIT_FOLD_CLAUSES=0")

# The raising arm mono-inline-implicit-null-free leaves out of a callee's
# cost. The body is past the default cost-translate limit and past the
# default cold-callsite threshold, so both arms raise both -- the file says
# why.
_mono_exe_list(_tier2_inline_nullcheck ${MONO_TESTS_TIER2_INLINE_NULLCHECK_SRC})
mono_runtime_suite(runtime-tier2-inline-nullcheck TESTS ${_tier2_inline_nullcheck}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=512"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inlinedefault-threshold=1200 --llvm-opt=-mono-inline-cold-callsite-threshold=700")
mono_runtime_suite(runtime-tier2-inline-nullcheck-off TESTS ${_tier2_inline_nullcheck}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=512"
                       "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inlinedefault-threshold=1200 --llvm-opt=-mono-inline-cold-callsite-threshold=700 --llvm-opt=-mono-inline-implicit-null-free=false")

# The scalarize-arg-bonus, on a callee that only reads an uncaptured
# argument's fields. The off arm leaves the other bonuses alone, because what
# it separates is this one from the fold tier2-inline-policy.cs's Measure ()
# takes with the argument bonus instead.
_mono_exe_list(_tier2_inline_scalarize ${MONO_TESTS_TIER2_INLINE_SCALARIZE_SRC})
mono_runtime_suite(runtime-tier2-inline-scalarize TESTS ${_tier2_inline_scalarize}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=200")
mono_runtime_suite(runtime-tier2-inline-scalarize-off TESTS ${_tier2_inline_scalarize}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=200 --llvm-opt=-mono-inline-scalarize-arg-bonus=0")

# What the cost model answers about a receiver the call site allocated. The off
# arm turns those answers off and leaves the bonuses alone, because what it
# separates is the fold rather than a threshold.
_mono_exe_list(_tier2_inline_dispatch ${MONO_TESTS_TIER2_INLINE_DISPATCH_SRC})
mono_runtime_suite(runtime-tier2-inline-dispatch TESTS ${_tier2_inline_dispatch}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=256"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=190")
mono_runtime_suite(runtime-tier2-inline-dispatch-off TESTS ${_tier2_inline_dispatch}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=256"
                       "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=190 --llvm-opt=-mono-inline-dispatch-is-a-load=false --llvm-opt=-mono-inline-fold-vtable-fields=false")

# What the cost model answers about a type test over a parameter. The off arm
# turns that answer off alone, so what it separates is the answered cascade.
_mono_exe_list(_tier2_inline_casts ${MONO_TESTS_TIER2_INLINE_CASTS_SRC})
mono_runtime_suite(runtime-tier2-inline-casts TESTS ${_tier2_inline_casts}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=512"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=400")
mono_runtime_suite(runtime-tier2-inline-casts-off TESTS ${_tier2_inline_casts}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=512"
                       "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=400 --llvm-opt=-mono-inline-answer-casts=false")

# A wrapper folded into its caller. The off arm leaves the cost model nothing to
# translate, which is what separates the fold from the frame: both arms assert
# that the wrapper still has a frame, and only the fold moves its offset onto the
# caller's. The site is cold, so both name a threshold that reaches it.
_mono_exe_list(_tier2_inline_wrapper ${MONO_TESTS_TIER2_INLINE_WRAPPER_SRC})
mono_runtime_suite(runtime-tier2-inline-wrapper TESTS ${_tier2_inline_wrapper}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=400")
mono_runtime_suite(runtime-tier2-inline-wrapper-off TESTS ${_tier2_inline_wrapper}
                   ENV "MONO_LLVM_JIT_TIER2_THRESHOLD=0"
                       "MONO_LLVM_JIT_INLINE_IL_LIMIT=0"
                       "MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=0"
                       "MONO_WRAPPER_FOLD=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-cold-callsite-threshold=400")

# MONO_ENV_OPTIONS has to reach the runtime before it parses its own argv.
foreach(_gc IN LISTS _mono_gcs)
  _mono_gc_env(_gc_env "${_gc}")
  add_test(NAME "runtime-env-options@${_gc}"
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                   "${_gc_env}"
                   "MONO_ENV_OPTIONS=--version" "${_wrapper}" array-init.exe
           WORKING_DIRECTORY "${_bin}")
  set_tests_properties("runtime-env-options@${_gc}" PROPERTIES
    LABELS runtime
    PASS_REGULAR_EXPRESSION "Architecture:")
endforeach()

# eglib's symbols are remapped to monoeg_* so that a runtime linked into a host
# that already has glib does not collide with it. Anything still exported as a
# bare g_* is a missed entry in eglib-remap.h.
# It is a property of each binary that was linked, so check each of them.
if(MONO_NM)
  foreach(_gc IN LISTS _mono_gcs)
    add_test(NAME "runtime-eglib-remap@${_gc}"
             COMMAND "${CMAKE_COMMAND}"
                     "-DNM=${MONO_NM}"
                     "-DBINARY=${CMAKE_BINARY_DIR}/mono/mini/mono-${_gc}"
                     -P "${CMAKE_SOURCE_DIR}/cmake/MonoCheckEglibRemap.cmake")
    set_tests_properties("runtime-eglib-remap@${_gc}" PROPERTIES LABELS runtime)
  endforeach()
endif()

mono_runtime_suite(runtime-internalsvisibleto
  TESTS internalsvisibleto-runtimetest.exe internalsvisibleto-compilertest.exe
        internalsvisibleto-runtimetest-sign2048.exe
        internalsvisibleto-compilertest-sign2048.exe)

mono_runtime_suite(runtime-ignoresaccesschecks
  TESTS ignoresaccesschecks-test.exe)

# valid-only, because corlib is not verifiable and never has been: it is full of
# localloc and native pointers, and Roslyn no longer emits the verifiable
# encodings of the rest. What the check is worth is that none of it is *invalid*.
mono_runtime_check(runtime-pedump NATIVE
  COMMAND "${CMAKE_BINARY_DIR}/tools/pedump/pedump" --verify code,metadata,valid-only
          "${_class_dir}/mscorlib.dll")

# The IL verifier, which --security=validil turns on.
#
# verification-invalid-il.exe calls a method whose body is invalid but which
# both engines happily run, and prints "ran 42" or "rejected <exception>". The
# four arms are the two security settings crossed with the two tiers: the flag
# is what makes the difference, and the tier is what must not.
#
# Some tier-0 arms name the invalid method rather than taking every method. That
# keeps its caller compiled, so the call goes through the stub the backend
# published and the backend is what answers for the callee. The unrestricted
# arms below are the ones that matter more: with nothing named, the caller is
# interpreted too and reaches its callee without the backend ever being asked,
# so the verdict has to come from the interpreter's own path.
function(_mono_verification_check name expect)
  cmake_parse_arguments(ARG "" "PROGRAM" "ARGS;ENV;REJECT" ${ARGN})
  if(NOT ARG_PROGRAM)
    set(ARG_PROGRAM verification-invalid-il.exe)
  endif()
  foreach(_gc IN LISTS _mono_gcs)
    _mono_gc_env(_gc_env "${_gc}")
    add_test(NAME "${name}@${_gc}"
             COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                     "${_gc_env}" "MONO_LLVM_JIT_TRACE=1" ${ARG_ENV}
                     "${_wrapper}" ${ARG_ARGS} "${ARG_PROGRAM}"
             WORKING_DIRECTORY "${_bin}")
    set_tests_properties("${name}@${_gc}" PROPERTIES
      LABELS runtime TIMEOUT 300
      PASS_REGULAR_EXPRESSION "${expect}"
      FAIL_REGULAR_EXPRESSION "${ARG_REJECT}")
  endforeach()
endfunction()

_mono_verification_check(runtime-verification-off "ran 42"
                         ENV "MONO_LLVM_JIT_TIER0=0" REJECT "rejected")
_mono_verification_check(runtime-verification-validil
                         "rejected System.InvalidProgramException"
                         ARGS --security=validil
                         ENV "MONO_LLVM_JIT_TIER0=0" REJECT "ran 42")
_mono_verification_check(runtime-verification-tier0
                         "interpreting Probe:Unverifiable"
                         ENV "MONO_LLVM_JIT_TIER0=Probe:Unverifiable"
                         REJECT "rejected")
# Rejecting without ever printing the routing line is the placement itself: the
# verdict is reached before the tier is chosen.
_mono_verification_check(runtime-verification-validil-tier0
                         "rejected System.InvalidProgramException"
                         ARGS --security=validil
                         ENV "MONO_LLVM_JIT_TIER0=Probe:Unverifiable"
                         REJECT "ran 42|interpreting Probe:Unverifiable")

# Tier 0 unrestricted, so the caller is interpreted as well and reaches the
# callee without the backend being asked for it. The verdict has to come from
# the interpreter transforming the callee.
_mono_verification_check(runtime-verification-validil-interpreted
                         "rejected System.InvalidProgramException"
                         ARGS --security=validil
                         REJECT "ran 42")

# The same, for a callee small enough to be inlined. An inlined body never gets
# a transform of its own, so it is checked where the interpreter decides to
# inline it - a body that does not verify is not inlined, and is then verified
# as an ordinary callee.
_mono_verification_check(runtime-verification-inlined
                         "rejected System.Security.VerificationException"
                         PROGRAM verification-inlined-il.exe
                         ARGS --verify-all
                         REJECT "ran 42")
_mono_verification_check(runtime-verification-inlined-off "ran 42"
                         PROGRAM verification-inlined-il.exe
                         REJECT "rejected")
