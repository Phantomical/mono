# A second assembly for the tests that convert and deserialize types the test
# assembly does not itself define.  It is loaded from beside the test assembly
# by name, so it goes into the tests directory rather than the fixture one.
mono_test_fixture_assembly(
  PROFILE  net_4_x
  ASSEMBLY System.Windows.Forms.dll
  NAME     DummyAssembly.dll
  IN_TESTS_DIR
  REFS     System
  SOURCES  Test/DummyAssembly/AnotherSerializable.cs
           Test/DummyAssembly/Convertable.cs
           Test/DummyAssembly/Properties/AssemblyInfo.cs)
