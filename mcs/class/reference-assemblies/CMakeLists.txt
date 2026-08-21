# This directory builds nothing.  The reference assemblies are checked into
# external/binary-reference-assemblies and only get copied into the versioned
# *-api directories that msbuild and the IDEs read.
set(_src "${CMAKE_SOURCE_DIR}/external/binary-reference-assemblies")
set(_dest "${CMAKE_INSTALL_LIBDIR}/mono")

set(_versions 2.0 3.5 4.0 4.5 4.5.1 4.5.2 4.6 4.6.1 4.6.2 4.7 4.7.1 4.7.2 4.8)
# The Facades split only exists from 4.5 on.
set(_facade_versions 4.5 4.5.1 4.5.2 4.6 4.6.1 4.6.2 4.7 4.7.1 4.7.2 4.8)
# mono's own reference assemblies go into every api directory except 3.5,
# which predates them.
set(_mono_versions 2.0 4.0 4.5 4.5.1 4.5.2 4.6 4.6.1 4.6.2 4.7 4.7.1 4.7.2 4.8)

foreach(_v IN LISTS _versions)
  file(GLOB _dlls "${_src}/v${_v}/*.dll")
  install(PROGRAMS ${_dlls} DESTINATION "${_dest}/${_v}-api" COMPONENT mcs)
endforeach()

foreach(_v IN LISTS _facade_versions)
  file(GLOB _dlls "${_src}/v${_v}/Facades/*.dll")
  install(PROGRAMS ${_dlls} DESTINATION "${_dest}/${_v}-api/Facades" COMPONENT mcs)
endforeach()

file(GLOB _mono_dlls "${_src}/mono/*.dll")
# From 4.7.1 on the framework itself carries SharpZipLib, so mono's copy is
# left out there to avoid shadowing it.
list(FILTER _mono_dlls EXCLUDE REGEX "/ICSharpCode\\.SharpZipLib\\.dll$")
file(GLOB _sharpzip "${_src}/mono/ICSharpCode.SharpZipLib.dll")
foreach(_v IN LISTS _mono_versions)
  install(PROGRAMS ${_mono_dlls} DESTINATION "${_dest}/${_v}-api" COMPONENT mcs)
  if(NOT _v MATCHES "^4\\.(7\\.1|7\\.2|8)$")
    install(PROGRAMS ${_sharpzip} DESTINATION "${_dest}/${_v}-api" COMPONENT mcs)
  endif()
endforeach()

# NUnit and FSharp have hardcoded checks for <prefix>/lib/mono/4.0/mscorlib.dll
# and Mono.Posix.dll.  No other install() rule writes into 4.0, so these two
# are symlinked to the reference assembly to keep those tools working.
# See https://bugzilla.xamarin.com/show_bug.cgi?id=38331 and .../41052
install(CODE "
set(_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_dest}/4.0\")
file(MAKE_DIRECTORY \"\${_dir}\")
foreach(_n mscorlib.dll Mono.Posix.dll)
  file(CREATE_LINK \"../4.0-api/\${_n}\" \"\${_dir}/\${_n}\" SYMBOLIC)
endforeach()
" COMPONENT mcs)
