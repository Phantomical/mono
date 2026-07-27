# The CIL opcode tables.  opcode.def is checked in and only regenerated on
# demand: the generator needs perl and XML::Parser, which CI does not have.

install(FILES opcode.def DESTINATION "${MONO_INCLUDE_SUBDIR}/mono/cil")
install(FILES cil-opcodes.xml DESTINATION "${MONO_DATA_SUBDIR}/mono/cil")

find_program(MONO_PERL perl)
if(MONO_PERL)
  add_custom_target(regen-opcode-def
    COMMAND "${MONO_PERL}" "${CMAKE_CURRENT_SOURCE_DIR}/make-opcodes-def.pl"
            "${CMAKE_CURRENT_SOURCE_DIR}/cil-opcodes.xml"
            "${CMAKE_CURRENT_SOURCE_DIR}/opcode.def"
    COMMENT "Regenerating mono/cil/opcode.def"
    VERBATIM)
endif()
