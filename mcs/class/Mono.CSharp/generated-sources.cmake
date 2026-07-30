# The C# grammar, shared with mcs.exe.  One copy serves every profile: nothing
# about the generated parser is profile-dependent.
mono_jay_parser(
  TARGET  mono-csharp-parser
  OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/cs-parser.cs"
  GRAMMAR "${MONO_MCS_TOPDIR}/mcs/cs-parser.jay"
  FLAGS   -c)

set(MCS_GENERATED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/cs-parser.cs")
set(MCS_GENERATED_TARGETS mono-csharp-parser)
