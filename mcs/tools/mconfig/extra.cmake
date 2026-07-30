# The only thing mcs installs outside $prefix/lib.
install(FILES data/config.xml
        DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/mono/mconfig"
        COMPONENT mcs)
