# xbuild's data files: a per-version set that follows each xbuild binary, and
# a set of shared trees installed once.
set(_libdir "${CMAKE_INSTALL_LIBDIR}")

# -- per xbuild version ------------------------------------------------------
# net_4_x builds xbuild 4.0 and lands it in lib/mono/4.5 beside the compiler.
# The xbuild_1x profiles get their own bin directory.
function(_xbuild_bin_data version dest)
  install(FILES data/xbuild.rsp
                data/Microsoft.Build.xsd
                data/Microsoft.VisualBasic.targets
                "data/${version}/Microsoft.Common.tasks"
                "data/${version}/Microsoft.Common.targets"
                "data/${version}/Microsoft.CSharp.targets"
          DESTINATION "${dest}" COMPONENT mcs)
  install(FILES data/MSBuild/Microsoft.Build.CommonTypes.xsd
                data/MSBuild/Microsoft.Build.Core.xsd
          DESTINATION "${dest}/MSBuild" COMPONENT mcs)
  # xbuild.exe.config names the assembly version of the Microsoft.Build.*
  # assemblies this xbuild binds to, which is why it is per-version.
  set(ASM_VERSION "${version}.0.0")
  configure_file(data/xbuild.exe.config.in
                 "${CMAKE_CURRENT_BINARY_DIR}/${version}/xbuild.exe.config" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${version}/xbuild.exe.config"
          DESTINATION "${dest}" COMPONENT mcs)
endfunction()

_xbuild_bin_data(4.0  "${_libdir}/mono/4.5")
_xbuild_bin_data(12.0 "${_libdir}/mono/xbuild/12.0/bin")
_xbuild_bin_data(14.0 "${_libdir}/mono/xbuild/14.0/bin")

# Only 14.0 has it, and it sits one level above the bin directory.
install(FILES data/14.0/Microsoft.Common.props
        DESTINATION "${_libdir}/mono/xbuild/14.0" COMPONENT mcs)

# -- the shared trees --------------------------------------------------------
set(_nuget "${MONO_MCS_TOPDIR}/../external/nuget-buildtasks/src/Microsoft.NuGet.Build.Tasks")

# The NuGet import hooks are installed under both the versioned tree and the
# version-independent `Current` one, which is where msbuild looks.
foreach(_root "${_libdir}/mono/xbuild/14.0" "${_libdir}/mono/xbuild/Current")
  install(FILES "${_nuget}/ImportBeforeAfter/Microsoft.NuGet.ImportBefore.props"
          DESTINATION "${_root}/Imports/Microsoft.Common.props/ImportBefore"
          COMPONENT mcs)
  install(FILES "${_nuget}/ImportBeforeAfter/Microsoft.NuGet.ImportAfter.targets"
          DESTINATION "${_root}/Microsoft.Common.targets/ImportAfter"
          COMPONENT mcs)
endforeach()

install(FILES "${_nuget}/Microsoft.NuGet.targets" "${_nuget}/Microsoft.NuGet.props"
        DESTINATION "${_libdir}/mono/xbuild/Microsoft/NuGet" COMPONENT mcs)

install(FILES data/deniedAssembliesList.txt
        DESTINATION "${_libdir}/mono/xbuild" COMPONENT mcs)

# The framework lists msbuild reads to decide what a target framework contains.
# Each is installed under its own version directory and renamed.
set(_fxdir "${_libdir}/mono/xbuild-frameworks/.NETFramework")
foreach(_v 2.0 3.0 3.5 4.0 4.5 4.5.1 4.5.2 4.6 4.6.1 4.6.2 4.7 4.7.1 4.7.2 4.8)
  install(FILES "frameworks/net_${_v}.xml"
          DESTINATION "${_fxdir}/v${_v}/RedistList"
          RENAME FrameworkList.xml COMPONENT mcs)
endforeach()
install(FILES frameworks/net_4.0_client.xml
        DESTINATION "${_fxdir}/v4.0/Profile/Client/RedistList"
        RENAME FrameworkList.xml COMPONENT mcs)

# Portable class libraries.
set(_pcl "${_libdir}/mono/xbuild/Microsoft/Portable")
install(FILES data/Portable/Targets/Microsoft.Portable.Core.props
              data/Portable/Targets/Microsoft.Portable.Core.targets
        DESTINATION "${_pcl}" COMPONENT mcs)
foreach(_v v4.0 v4.5 v4.6 v5.0)
  install(FILES "data/Portable/Targets/${_v}/Microsoft.Portable.Common.targets"
                "data/Portable/Targets/${_v}/Microsoft.Portable.CSharp.targets"
                "data/Portable/Targets/${_v}/Microsoft.Portable.VisualBasic.targets"
          DESTINATION "${_pcl}/${_v}" COMPONENT mcs)
endforeach()

set(_pcl5 "${_libdir}/mono/xbuild-frameworks/.NETPortable/v5.0")
install(FILES data/Portable/Frameworks/v5.0/FrameworkList.xml
        DESTINATION "${_pcl5}/RedistList" COMPONENT mcs)
# Listed rather than globbed: one of these three begins with a dot.
install(FILES "data/Portable/Frameworks/v5.0/.NET Framework 4.6.xml"
              "data/Portable/Frameworks/v5.0/ASP.NET Core 1.0.xml"
              "data/Portable/Frameworks/v5.0/Windows Universal 10.0.xml"
        DESTINATION "${_pcl5}/SupportedFrameworks" COMPONENT mcs)

# The web-application targets, which every Visual Studio version looks for
# under its own directory.  v13.0 never existed.
foreach(_v v9.0 v10.0 v11.0 v12.0 v14.0 v15.0 v16.0)
  install(FILES targets/Microsoft.WebApplication.targets
          DESTINATION "${_libdir}/mono/xbuild/Microsoft/VisualStudio/${_v}/WebApplications"
          COMPONENT mcs)
endforeach()
