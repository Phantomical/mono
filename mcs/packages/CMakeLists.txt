# Hand-written: this directory builds nothing.  It installs the checked-in
# Roslyn binaries -- the same ones this build compiles with -- so that the
# installed mono has a working csc.
get_filename_component(_roslyn "${MONO_CSC}" DIRECTORY)

# Everything mono itself needs to run the compiler.
set(_for_mono
    csc.exe csc.rsp csc.exe.config
    csi.exe csi.exe.config csi.rsp
    vbc.exe vbc.exe.config vbc.rsp
    Microsoft.CodeAnalysis.CSharp.dll
    Microsoft.CodeAnalysis.CSharp.Scripting.dll
    Microsoft.CodeAnalysis.VisualBasic.dll
    Microsoft.CodeAnalysis.dll
    Microsoft.CodeAnalysis.Scripting.dll
    System.Collections.Immutable.dll
    System.Reflection.Metadata.dll
    System.Runtime.CompilerServices.Unsafe.dll
    System.Threading.Tasks.Extensions.dll
    System.Memory.dll
    VBCSCompiler.exe VBCSCompiler.exe.config)

set(_dest "${CMAKE_INSTALL_LIBDIR}/mono/4.5")
set(_roslyn_dest "${CMAKE_INSTALL_LIBDIR}/mono/msbuild/Current/bin/Roslyn")

foreach(_f IN LISTS _for_mono)
  install(PROGRAMS "${_roslyn}/${_f}" DESTINATION "${_dest}" COMPONENT mcs)
endforeach()

# msbuild wants the compiler under its own tree.  The task assembly and the
# .targets files are real copies.  The compiler itself is symlinked back to
# the one above rather than duplicated.
install(PROGRAMS
        "${_roslyn}/Microsoft.Build.Tasks.CodeAnalysis.dll"
        "${_roslyn}/Microsoft.CSharp.Core.targets"
        "${_roslyn}/Microsoft.Managed.Core.targets"
        "${_roslyn}/Microsoft.VisualBasic.Core.targets"
        DESTINATION "${_roslyn_dest}" COMPONENT mcs)

string(JOIN " " _names ${_for_mono})
install(CODE "
set(_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_roslyn_dest}\")
file(MAKE_DIRECTORY \"\${_dir}\")
# Relative, and four levels up: Roslyn/ -> bin/ -> Current/ -> msbuild/ -> mono/
foreach(_n ${_names})
  file(CREATE_LINK \"../../../../4.5/\${_n}\" \"\${_dir}/\${_n}\" SYMBOLIC)
endforeach()
" COMPONENT mcs)

include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake)
