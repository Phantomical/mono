MAKEFLAGS := $(MAKEFLAGS) --no-builtin-rules

#
# jemalloc is a git submodule at mono/utils/jemalloc/jemalloc, marked
# 'update = none' in .gitmodules because only --with-jemalloc needs it.  Get it
# with:
#
#   git submodule update --init --checkout mono/utils/jemalloc/jemalloc
#

JEMALLOC_PATH=jemalloc

# No `$(RM) -r $(JEMALLOC_PATH)` here: that directory is a submodule checkout,
# and wiping it would need a fetch to get back.
clean-local:

if MONO_JEMALLOC_ASSERT
ASSERT_OPT=--enable-debug
endif

jemalloc:
	@echo "*** [jemalloc] checkout missing; run:"
	@echo "***   git submodule update --init --checkout mono/utils/jemalloc/jemalloc"
	@exit 1

# Set a prefix to enable access to allocation functions with a prefix, ie so mono_jemalloc isn't named malloc by default
# Disable zone allocator, otherwise malloc uses jemalloc for things it's not set for
# We call autoconf ourselves so we can call configure and not autogen. Autogen script is broken, minor bash issues around quote escaping
jemalloc/lib/libjemalloc.a: jemalloc Makefile
	cd jemalloc && autoconf && ./configure --with-jemalloc-prefix=mono_je --prefix=`pwd` --enable-debug $(JEMALLOC_AUTOCONF_FLAGS) --disable-zone-allocator EXTRA_CFLAGS="-I $(top_srcdir) $(GLIB_CFLAGS) $(CFLAGS) $(PLATFORM_CFLAGS) $(ARCH_CFLAGS) $(SHARED_CFLAGS) -Wno-error" CC="$(CC)" CXX="$(CXX)" CPPFLAGS="$(CPPFLAGS) $(JEMALLOC_CPPFLAGS) " CXXFLAGS="$(CXXFLAGS)" LDFLAGS="$(LDFLAGS)"

	cd jemalloc && $(MAKE) build_lib_static 

all-local: jemalloc/lib/libjemalloc.a


