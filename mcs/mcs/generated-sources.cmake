# The C# grammar.  Switch FLAGS to -cvt for a tracing parser.
mono_jay_parser(
  TARGET  mcs-cs-parser
  OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/cs-parser.cs"
  GRAMMAR cs-parser.jay
  FLAGS   -c)

set(MCS_GENERATED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/cs-parser.cs")
set(MCS_GENERATED_TARGETS mcs-cs-parser)
