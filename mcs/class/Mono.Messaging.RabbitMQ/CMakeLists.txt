# Converted from class/Mono.Messaging.RabbitMQ/Makefile.

mono_declare_managed(
  NAME       Mono.Messaging.RabbitMQ.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  REFS       System System.Messaging Mono.Messaging RabbitMQ.Client
  FLAGS      -nowarn:618
)
