# getline.cs ships as source: it is meant to be copied into other programs,
# which is what mono-source-libs is for.
install(FILES getline.cs
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/mono-source-libs"
        COMPONENT mcs)
