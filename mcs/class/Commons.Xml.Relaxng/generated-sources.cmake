# The RELAX NG compact-syntax parser.
#
# The makefile wrote RncParser.cs next to its grammar. This writes it to the
# build tree instead. That works because a generated source is passed to csc
# on the command line, not listed in a .sources file.
mono_jay_parser(
  TARGET  relaxng-rnc-parser
  OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/RncParser.cs"
  GRAMMAR Commons.Xml.Relaxng.Rnc/RncParser.jay
  FLAGS   -ctv)

set(MCS_GENERATED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/RncParser.cs")
set(MCS_GENERATED_TARGETS relaxng-rnc-parser)
