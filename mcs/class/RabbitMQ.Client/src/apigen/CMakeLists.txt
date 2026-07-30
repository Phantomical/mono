mono_declare_managed(
  PROGRAM
  NAME       RabbitMQ.Client.Apigen.exe
  PROFILES   net_4_x unityjit
  REFS       System System.Xml
  FLAGS      /main:RabbitMQ.Client.Apigen.Apigen
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
