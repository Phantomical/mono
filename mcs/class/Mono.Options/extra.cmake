# Mono.Options.dll itself is NO_INSTALL -- the option parser is meant to be
# vendored as source, so only Options.cs ships.  The makefile installed it once
# per profile that built the directory; one rule covers them all here.
install(FILES Mono.Options/Options.cs
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/mono-source-libs"
        COMPONENT mcs)
