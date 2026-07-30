# The three AMQP protocol bindings, generated from the stripped spec XML by
# Apigen, which this same profile builds from src/apigen.  One copy serves
# every profile: the output depends on the spec, not on who is compiling.
set(MCS_GENERATED_SOURCES "")
set(MCS_GENERATED_TARGETS "")

set(_specs "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/specs")

function(_rabbitmq_api suffix xml ns apiname)
  set(_out "${CMAKE_CURRENT_BINARY_DIR}/api-${suffix}.cs")
  mono_generated_source(
    TARGET       rabbitmq-api-${suffix}
    OUTPUT       "${_out}"
    PROFILE      net_4_x
    TOOL         RabbitMQ.Client.Apigen.exe
    TOOL_PROFILE net_4_x
    ARGS         "/n:${ns}" "/apiName:${apiname}"
                 "${_specs}/${xml}.stripped.xml" "${_out}"
    DEPENDS      "${_specs}/${xml}.stripped.xml")
  set(MCS_GENERATED_SOURCES ${MCS_GENERATED_SOURCES} "${_out}" PARENT_SCOPE)
  set(MCS_GENERATED_TARGETS ${MCS_GENERATED_TARGETS} rabbitmq-api-${suffix}
      PARENT_SCOPE)
endfunction()

_rabbitmq_api(0-9      amqp0-9        v0_9     AMQP_0_9)
_rabbitmq_api(0-8      amqp0-8        v0_8     AMQP_0_8)
_rabbitmq_api(qpid-0-8 qpid-amqp.0-8  v0_8qpid AMQP_0_8_QPID)
