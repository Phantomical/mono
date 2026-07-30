mono_declare_managed(
  PROGRAM
  NAME       svcutil.exe
  PROFILES   net_4_x
  REFS       System.Core System.Runtime.Serialization System.ServiceModel System.Web.Services System.Configuration System System.Xml
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
