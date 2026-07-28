# The Helix CI upload client.
#
# Hand-written because its references are paths into a submodule rather than
# assembly names, and because the Makefile's targets submit CI jobs rather than
# build anything.  Declared only when that submodule is checked out.
if(EXISTS "${CMAKE_SOURCE_DIR}/external/helix-binaries/Microsoft.DotNet.Helix.Client.dll")
  set(_helix "${CMAKE_SOURCE_DIR}/external/helix-binaries")
  mono_declare_managed(
    PROGRAM
    NAME     mono-helix-client.exe
    PROFILES build
    REFS     System System.Net.Http Facades/netstandard Facades/System.Runtime
             Facades/System.Threading.Tasks
    FLAGS    "-r:${_helix}/Microsoft.DotNet.Helix.Client.dll"
             "-r:${_helix}/Microsoft.DotNet.Helix.JobSender.dll"
             "-r:${_helix}/Microsoft.Rest.ClientRuntime.dll"
    NO_INSTALL)
endif()
