# monodoc.dll.config points the library at the installed documentation tree,
# so the install prefix has to be substituted in.  The makefile did it with
# sed.  The template's placeholder is already @-delimited.
set(monodoc_refdir "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/monodoc")
foreach(_p IN ITEMS net_4_x unityjit)
  mono_profile_dir(_dir ${_p})
  configure_file(monodoc.dll.config.in "${_dir}/monodoc.dll.config" @ONLY)
endforeach()
