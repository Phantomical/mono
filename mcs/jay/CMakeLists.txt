# The parser generator the C# compiler and ilasm are built with.  Native C, and
# the only thing under mcs/ that is not a managed assembly.
#
# jay always reads the skeleton off stdin, with no file fallback, and every
# caller in this tree already redirects one in.  main.c prints SKEL_DIRECTORY
# behind the -p flag, so the macro must stay defined.  That is the only
# reason it is kept.
add_executable(jay
  closure.c error.c lalr.c lr0.c main.c mkpar.c output.c reader.c
  symtab.c verbose.c warshall.c)
target_compile_definitions(jay PRIVATE
  "SKEL_DIRECTORY=\"${CMAKE_INSTALL_PREFIX}/share/jay\"")
# Not installed: this is a build tool, and the makefiles never reached the
# install rule for it either -- jay only ever ran in the bootstrap profile,
# which installs nothing.
