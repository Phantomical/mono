# UplevelHelper.cs is a lookup table compiled from the browser-capability
# definitions by culevel, which the bootstrap profile builds.
mono_generated_source(
  TARGET  system-web-uplevel-helper
  OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/UplevelHelper.cs"
  PROFILE net_4_x
  TOOL    culevel.exe
  ARGS    -o "${CMAKE_CURRENT_BINARY_DIR}/UplevelHelper.cs"
          "${CMAKE_CURRENT_SOURCE_DIR}/UplevelHelperDefinitions.xml"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/UplevelHelperDefinitions.xml")

set(MCS_GENERATED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/UplevelHelper.cs")
set(MCS_GENERATED_TARGETS system-web-uplevel-helper)
