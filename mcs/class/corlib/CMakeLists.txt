# mscorlib.
#
# The one assembly that is not just a compile.  csc emits it, then
# cil-stringreplacer splices in two things: the resource string table from
# referencesource, and an IL module ilasm built from Unsafe.il for the
# construct csc cannot express.  What ships is therefore rewritten IL.
#
# Everything else in the tree waits on this, in every profile.

# The IL module, per profile -- it is spliced into that profile's mscorlib, so
# it cannot be shared between them.
set(_corlib_deps "")
foreach(_p build net_4_x unityjit)
  mono_profile_dir(_dir ${_p})
  mono_add_il_module(
    TARGET  corlib-unsafe-${_p}
    OUTPUT  "${_dir}/corlib.unsafe.dll.tmp"
    SOURCES System.Runtime.CompilerServices/Unsafe.il
    PROFILE ${_p}
    FLAGS   /dll /noautoinherit)
endforeach()

# The reference-source feature matrix.  In the makefiles this was a dozen
# nested ifdefs over knobs the mobile profiles set; none of those profiles
# survive, so every branch is taken and the result is a constant.
set(_refsrc -d:FEATURE_PAL,GENERICS_WORK,FEATURE_LIST_PREDICATES,FEATURE_SERIALIZATION,FEATURE_ENCODINGNLS,FEATURE_ASCII,FEATURE_LATIN1,FEATURE_UTF7,FEATURE_UTF32,MONO_HYBRID_ENCODING_SUPPORT,FEATURE_ASYNC_IO,NEW_EXPERIMENTAL_ASYNC_IO,FEATURE_UTF32,FEATURE_EXCEPTIONDISPATCHINFO,FEATURE_CORRUPTING_EXCEPTIONS,FEATURE_EXCEPTION_NOTIFICATIONS,FEATURE_STRONGNAME_MIGRATION,FEATURE_USE_LCID,FEATURE_FUSION,FEATURE_CRYPTO,FEATURE_X509_SECURESTRINGS,FEATURE_SYNCHRONIZATIONCONTEXT,FEATURE_SYNCHRONIZATIONCONTEXT_WAIT,FEATURE_DEFAULT_INTERFACES,HAS_CORLIB_CONTRACTS
            -d:FEATURE_MACL
            -d:FEATURE_REMOTING,MONO_COM,FEATURE_COMINTEROP,FEATURE_ROLE_BASED_SECURITY
            -d:MONO_FEATURE_THREAD_ABORT
            -d:MONO_FEATURE_THREAD_SUSPEND_RESUME
            -d:MONO_FEATURE_MULTIPLE_APPDOMAINS)

set(_resources
    resources/collation.core.bin resources/collation.tailoring.bin
    resources/collation.cjkCHS.bin resources/collation.cjkCHT.bin
    resources/collation.cjkJA.bin resources/collation.cjkKO.bin
    resources/collation.cjkKOlv2.bin
    LinkerDescriptor/mscorlib.xml)
set(_resource_flags "")
foreach(_r IN LISTS _resources)
  list(APPEND _resource_flags "-resource:${_r}")
endforeach()

set(_common_flags
    ${_refsrc} ${_resource_flags}
    -langversion:preview
    -runtimemetadataversion:v4.0.30319
    -d:MONO_FEATURE_SRE
    -d:MONO_FEATURE_CONSOLE
    # LOCAL_MCS_FLAGS
    -unsafe -nostdlib
    -nowarn:612,618,3001,3002,3003,1635
    -d:INSIDE_CORLIB,MONO_CULTURE_DATA -d:LIBC
    -d:BIT64,REGISTRY_ASSEMBLY)

# The test suite's own resources.  Checked-in binaries the tests open by name,
# and one .resx that has to be compiled first.
set(_test_resources
    Test/resources/MyResources.resources
    Test/resources/MyResources.de.resources
    Test/resources/Empty.resources
    Test/resources/AFile.txt
    Test/resources/StreamTest.resources
    Test/resources/StreamTest.ja.resources
    Test/resources/bug81759.resources
    Test/resources/415628.bin
    Test/resources/Fergie.GED)

foreach(_p build net_4_x unityjit)
  mono_profile_dir(_dir ${_p})
  set(_flags ${_common_flags})
  # Only net_4_x runs tests, and the fields have to stay off the bootstrap
  # profile in particular: compiling a .resx needs resgen, which that profile
  # is what produces.
  set(_test "")
  if(_p STREQUAL "net_4_x")
    list(APPEND _flags -d:FEATURE_CRYPTO_CONFIGURABLE)
    set(_test
      RESX       Test/resources/Resources.resources
      TEST_REFS  System.Core System Mono.Posix
      TEST_RESOURCES ${_test_resources}
      TEST_FLAGS -nowarn:168,219,618,672 -unsafe
                 -nowarn:67,114,162,169,649,659
                 -define:MONO_DATACONVERTER_STATIC_METHODS
                 -d:FEATURE_CRYPTO_CONFIGURABLE
                 -resource:Test/resources/Resources.resources
                 "-resource:LinkerDescriptor/mscorlib_test.xml,${_p}_corlib_test.xml"
      XTEST_REFS System System.Core Facades/System.Threading.Tasks
                 Facades/System.Text.RegularExpressions System.Numerics
                 System.Runtime.CompilerServices.Unsafe
      XTEST_REMOTE_EXECUTOR
      XTEST_FLAGS -d:netcoreapp -publicsign -keyfile:../mono.snk
                  "-resource:${CMAKE_SOURCE_DIR}/external/corefx/src/System.Reflection/tests/Resources/ResourceTextFile.txt,ResourceTextFile.txt"
                  # A netmodule the reflection tests load from beside the test
                  # assembly; extra.cmake puts it there.
                  "-r:${_dir}/tests/System.Reflection.TestModule.dll")
  endif()
  mono_declare_managed(
    NAME        corlib.dll
    OUTPUT_NAME mscorlib.dll
    PROFILES    ${_p}
    NO_DEFAULT_REFERENCES
    ${_test}
    FLAGS       ${_flags}
    # The security manager reads its bootstrap database out of this directory
    # while compiling.
    ENV         "__SECURITY_BOOTSTRAP_DB=${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS     corlib-unsafe-${_p}
    STRING_REPLACER_FLAGS
                --mscorlib-debug
                "--resourcestrings:${CMAKE_CURRENT_SOURCE_DIR}/../referencesource/mscorlib/mscorlib.txt"
                "--ilreplace:${_dir}/corlib.unsafe.dll.tmp"
    INSTALL_DIR "lib/mono/4.5")
endforeach()

include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake)
