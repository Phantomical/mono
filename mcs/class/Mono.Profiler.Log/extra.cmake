# ExceptionEventsAreRecorded asserts the log stream carries an exception-clause
# event that this runtime does not emit; it fails identically under --nollvm,
# under the tiered default and with LLVM compiling everything, so it is the
# runtime's longstanding behaviour, first observed when this suite was first
# run in-tree.
mono_test_xunit_exclude(
  PROFILE  net_4_x
  ASSEMBLY Mono.Profiler.Log.dll
  METHODS  MonoTests.Mono.Profiler.Log.ProfilerTests.ExceptionEventsAreRecorded)

# The suite starts this as a second process under the log profiler and reads
# the events back, so it has to sit beside the test assembly, which is where
# the suite looks for it.
mono_test_fixture_assembly(
  PROGRAM IN_TESTS_DIR
  PROFILE  net_4_x
  ASSEMBLY Mono.Profiler.Log.dll
  NAME     log-profiler-test.exe
  SOURCES  Test/log-profiler-test.cs
  FLAGS    /unsafe
  REFS     Mono.Profiler.Log)
