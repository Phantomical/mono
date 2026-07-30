# mono-service is installed twice: as a program in lib/mono/4.5, which
# executable.make did, and into the GAC, which the makefile added on top so
# that a service host can bind to it by strong name.
mono_gac_install(PROFILE net_4_x NAME mono-service.exe)
