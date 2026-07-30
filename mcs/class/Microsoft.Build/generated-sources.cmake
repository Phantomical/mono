# The property/condition expression parser.
mono_jay_parser(
  TARGET  microsoft-build-expression-parser
  OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/ExpressionParser.cs"
  GRAMMAR Microsoft.Build.Internal/ExpressionParser.jay
  FLAGS   -ctv)

set(MCS_GENERATED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/ExpressionParser.cs")
set(MCS_GENERATED_TARGETS microsoft-build-expression-parser)
