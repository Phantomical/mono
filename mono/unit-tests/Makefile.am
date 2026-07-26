MAKEFLAGS := $(MAKEFLAGS) --no-builtin-rules

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/mono -I$(top_srcdir)/samples $(LIBGC_CPPFLAGS) $(GLIB_CFLAGS) -DMONO_BINDIR=\"$(bindir)/\" -DMONO_ASSEMBLIES=\"$(assembliesdir)\" -DMONO_CFG_DIR=\"$(confdir)\"

CLASS=$(mcs_topdir)/class/lib/$(DEFAULT_PROFILE)

TOOLS_RUNTIME = MONO_PATH=$(mcs_topdir)/class/lib/build $(top_builddir)/runtime/mono-wrapper --aot-path=$(mcs_topdir)/class/lib/build

MCS_NO_UNSAFE = $(TOOLS_RUNTIME) $(CSC) -debug:portable \
	-noconfig -nologo \
	-nowarn:0162 -nowarn:0168 -nowarn:0219 -nowarn:0414 -nowarn:0618 \
	-nowarn:0169 -nowarn:1690 -nowarn:0649 -nowarn:0612 -nowarn:3021 \
	-nowarn:0197 $(PROFILE_MCS_FLAGS)
MCS_NO_LIB = $(MCS_NO_UNSAFE) -unsafe

MCS = $(MCS_NO_LIB)

# The test executables statically pull in libmini, which contains C++/LLVM
# objects. Link them with the C++ compiler driver (like mono/mini does) so the
# C++ runtime (e.g. __gxx_personality_v0) is available.
CXXLD=$(MONO_CXXLD)
CCLD=$(MONO_CXXLD)

# Wire in the internal-LLVM link flags/libs the same way mono/mini does.
# Without this, LLVM_LIBS/LLVM_LDFLAGS are empty here and libmini's LLVM
# references (llvm::*, __jit_debug_*) go unresolved at link time.
if ENABLE_LLVM
include $(mono_build_root)/llvm/llvm_config.mk
# test-llvm-engine.cpp includes mono/mini/llvm/engine.hpp, which exposes LLVM
# C++ types, so it needs the same include path/-std as mono/mini uses for
# mono/mini/llvm/*.cpp.
LLVM_CXXFLAGS=$(LLVM_CXXFLAGS_INTERNAL) $(LLVM_CXXFLAGS_EXTERNAL)
LLVM_LDFLAGS=$(LLVM_LDFLAGS_INTERNAL) $(LLVM_LDFLAGS_EXTERNAL)
LLVM_LIBS=$(LLVM_LIBS_INTERNAL) $(LLVM_LIBS_EXTERNAL)

$(mono_build_root)/llvm/llvm_config.mk:
	$(MAKE) -C $(top_builddir)/llvm $(mono_build_root)/llvm/llvm_config.mk
endif

LLVMMONOF=$(LLVM_LIBS) $(LLVM_LDFLAGS)

glib_libs = $(top_builddir)/mono/eglib/libeglib.la

test_cflags = $(AM_CFLAGS) $(SGEN_DEFINES)
# NB: $(LLVMMONOF) is intentionally NOT here — the LLVM archives must appear
# on the link line *after* libmini (which references them), so they are added
# at the end of each program's LDADD below. Listing them here (before libmini)
# makes the linker discard them and the LLVM symbols go unresolved.
test_ldadd = libtestlib.la \
	$(LIBGC_LIBS) $(glib_libs)

if HOST_DARWIN
test_ldflags = -framework CoreFoundation -framework Foundation
endif

monodir=$(top_builddir)
sgen_libs = \
	$(monodir)/mono/metadata/libmonoruntimesgen.la	\
	$(monodir)/mono/sgen/libmonosgen.la	\
	$(monodir)/mono/utils/libmonoutils.la \
	$(glib_libs)

mini_libs = \
	$(monodir)/mono/mini/libmini.la

if !DISABLE_INTERPRETER
mini_libs += $(monodir)/mono/mini/libmono-ee-interp.la
endif

if !DISABLE_DEBUGGER_AGENT
mini_libs += $(monodir)/mono/mini/libmono-dbg.la
endif

# mini_libs/sgen_libs above name .la files built by OTHER top-level
# directories (mono/mini, mono/metadata, mono/sgen, mono/utils). Recursive
# make only tracks dependencies within the current directory's own Makefile:
# listing a foreign .la in _LDADD makes automake treat it as a *prerequisite*
# (so the link rule reruns if its mtime changes), but nothing here ever tells
# make to go check whether that .la itself needs rebuilding first. In
# practice that means `make -C mono/unit-tests check` can silently link
# against a stale libmini.la after e.g. mono/mini/llvm/mono_lsda.cpp changes,
# unless someone has separately run `make` under mono/mini since - the same
# hazard the llvm_config.mk rule below works around for the LLVM back end.
# Force the recursion unconditionally on every build; the sub-make in each
# target directory still does its own up-to-date check, so this costs a
# no-op sub-make invocation when nothing changed.
.PHONY: unit-tests-force-foreign-libs
$(monodir)/mono/mini/libmini.la: unit-tests-force-foreign-libs
	$(MAKE) -C $(monodir)/mono/mini libmini.la
$(monodir)/mono/mini/libmono-ee-interp.la: unit-tests-force-foreign-libs
	$(MAKE) -C $(monodir)/mono/mini libmono-ee-interp.la
$(monodir)/mono/mini/libmono-dbg.la: unit-tests-force-foreign-libs
	$(MAKE) -C $(monodir)/mono/mini libmono-dbg.la
$(monodir)/mono/metadata/libmonoruntimesgen.la: unit-tests-force-foreign-libs
	$(MAKE) -C $(monodir)/mono/metadata libmonoruntimesgen.la
$(monodir)/mono/sgen/libmonosgen.la: unit-tests-force-foreign-libs
	$(MAKE) -C $(monodir)/mono/sgen libmonosgen.la
$(monodir)/mono/utils/libmonoutils.la: unit-tests-force-foreign-libs
	$(MAKE) -C $(monodir)/mono/utils libmonoutils.la

CFLAGS = $(filter-out @CXX_REMOVE_CFLAGS@, @CFLAGS@)

if !CROSS_COMPILE
if !HOST_WIN32

noinst_LTLIBRARIES = libtestlib.la
libtestlib_la_CFLAGS = @CXX_ADD_CFLAGS@
libtestlib_la_SOURCES = \
	test-sgen-qsort.c \
	test-memfuncs.c \
	test-mono-linked-list-set.c \
	test-conc-hashtable.c \
	test-mono-handle.c \
	test-mono-callspec.c \
	test-mono-string.c \
	test-mono-embed.c \
	test-embed-invoke.c
if ENABLE_LLVM
# test-llvm-engine.cpp is the one C++ source in this otherwise-C library: it
# includes mono/mini/llvm/engine.hpp and drives the engine through its C++
# interface, so the engine's tests live here instead of shipping inside libmono.
# automake handles the mixed library on its own. libtestlib_la_CXXFLAGS applies
# to it alone (it is the only .cpp here) and supplies the -std=c++17 and include
# path LLVM 18 headers need - the same LLVM_CXXFLAGS mono/mini uses to build
# mono/mini/llvm/*.cpp.
libtestlib_la_SOURCES += test-llvm-engine.cpp
libtestlib_la_SOURCES += test-llvm-ehframe.c
# test-llvm-ehtable.cpp is C++ (like test-llvm-engine.cpp): it includes
# mono/mini/llvm/mono_lsda.hpp and drives the .mono_lsda parser/builder through
# its C++ interface.
libtestlib_la_SOURCES += test-llvm-ehtable.cpp
# test-llvm-class-init.cpp is C++ for the same reason: it includes
# mono/mini/llvm/passes/elide-class-init.hpp and builds LLVM IR by hand to drive
# the class-init redundancy analysis.
libtestlib_la_SOURCES += test-llvm-class-init.cpp
libtestlib_la_CXXFLAGS = $(LLVM_CXXFLAGS)
endif
libtestlib_la_LIBADD = ../metadata/libmonoruntimesgen.la ../sgen/libmonosgen.la ../utils/libmonoutils.la

test_sgen_qsort_SOURCES = main.c
test_sgen_qsort_CFLAGS = $(test_cflags) -DMAIN=test_sgen_qsort_main
test_sgen_qsort_LDADD = $(test_ldadd) libtestlib_la-test-sgen-qsort.lo
test_sgen_qsort_LDFLAGS = $(test_ldflags)

test_memfuncs_SOURCES = main.c
test_memfuncs_CFLAGS = $(test_cflags) -DMAIN=test_memfuncs_main
test_memfuncs_LDADD = $(test_ldadd) libtestlib_la-test-memfuncs.lo
test_memfuncs_LDFLAGS = $(test_ldflags)

test_mono_linked_list_set_SOURCES = main.c
test_mono_linked_list_set_CFLAGS = $(test_cflags) -DMAIN=test_mono_linked_list_set_main
test_mono_linked_list_set_LDADD = $(test_ldadd) libtestlib_la-test-mono-linked-list-set.lo
test_mono_linked_list_set_LDFLAGS = $(test_ldflags)

test_conc_hashtable_SOURCES = main.c
test_conc_hashtable_CFLAGS = $(test_cflags) -DMAIN=test_conc_hashtable_main
test_conc_hashtable_LDADD = $(test_ldadd) libtestlib_la-test-conc-hashtable.lo
test_conc_hashtable_LDFLAGS = $(test_ldflags)

test_mono_handle_SOURCES = main.c
test_mono_handle_CFLAGS = $(test_cflags) -DMAIN=test_mono_handle_main
test_mono_handle_LDADD = $(test_ldadd) libtestlib_la-test-mono-handle.lo
test_mono_handle_LDFLAGS = $(test_ldflags)

test_mono_callspec_SOURCES = main.c
test_mono_callspec_CFLAGS = $(AM_CFLAGS) -DMAIN=test_mono_callspec_main
test_mono_callspec_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF) libtestlib_la-test-mono-callspec.lo
test_mono_callspec_LDFLAGS = $(test_ldflags)
test_mono_callspec_DEPENDENCIES = callspec.exe

test_mono_string_SOURCES = main.c
test_mono_string_CFLAGS = $(test_cflags) -DMAIN=test_mono_string_main
test_mono_string_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF) libtestlib_la-test-mono-string.lo
test_mono_string_LDFLAGS = $(test_ldflags)

test_mono_embed_SOURCES = main.c
test_mono_embed_CFLAGS = $(test_cflags) -DMAIN=test_mono_embed_main
test_mono_embed_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF) libtestlib_la-test-mono-embed.lo
test_mono_embed_LDFLAGS = $(test_ldflags)
test_mono_embed_DEPENDENCIES = test-embed.exe

test_path_SOURCES = test-path.c
test_path_CFLAGS = $(test-cflags)
test_path_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF)
test_path_LDFLAGS = $(test_ldflags)

test_embed_invoke_SOURCES = main.c
test_embed_invoke_CFLAGS = $(test_cflags) -DMAIN=test_mono_embed_invoke_main
test_embed_invoke_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF) libtestlib_la-test-embed-invoke.lo
test_embed_invoke_LDFLAGS = $(test_ldflags)
test_embed_invoke_DEPENDENCIES = test-embed-invoke-cs.exe

# The LLVM engine tests. Statically pull in libmini (which contains engine.cpp)
# and link the internal LLVM archives via $(LLVMMONOF), same as the other
# mini-linked tests above.
if ENABLE_LLVM
test_llvm_engine_SOURCES = main.c
test_llvm_engine_CFLAGS = $(test_cflags) -DMAIN=test_llvm_engine_main
test_llvm_engine_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF) libtestlib_la-test-llvm-engine.lo
test_llvm_engine_LDFLAGS = $(test_ldflags)

# The relocation-width check gets its OWN program, built from the same object
# via main.c's -DMAIN shim. It is the one check that can legitimately not run
# (its relocation classifier covers x86-64 ELF only), and automake resolves one
# result per TESTS program: only a program of its own can exit 77 and be counted
# under "# SKIP:". Aggregated into test-llvm-engine it reported "# SKIP: 0" and
# a green board while testing nothing. See the entry-point comment in
# test-llvm-engine.cpp.
test_llvm_reloc_widths_SOURCES = main.c
test_llvm_reloc_widths_CFLAGS = $(test_cflags) -DMAIN=test_llvm_reloc_widths_main
test_llvm_reloc_widths_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF) libtestlib_la-test-llvm-engine.lo
test_llvm_reloc_widths_LDFLAGS = $(test_ldflags)

test_llvm_ehframe_SOURCES = main.c
test_llvm_ehframe_CFLAGS = $(test_cflags) -DMAIN=test_llvm_ehframe_main
test_llvm_ehframe_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF) libtestlib_la-test-llvm-ehframe.lo
test_llvm_ehframe_LDFLAGS = $(test_ldflags)

test_llvm_ehtable_SOURCES = main.c
test_llvm_ehtable_CFLAGS = $(test_cflags) -DMAIN=test_llvm_ehtable_main
test_llvm_ehtable_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF) libtestlib_la-test-llvm-ehtable.lo
test_llvm_ehtable_LDFLAGS = $(test_ldflags)

test_llvm_class_init_SOURCES = main.c
test_llvm_class_init_CFLAGS = $(test_cflags) -DMAIN=test_llvm_class_init_main
test_llvm_class_init_LDADD = $(test_ldadd) $(mini_libs) $(sgen_libs) $(LLVMMONOF) libtestlib_la-test-llvm-class-init.lo
test_llvm_class_init_LDFLAGS = $(test_ldflags)
endif


check_PROGRAMS = test-sgen-qsort test-memfuncs test-mono-linked-list-set test-conc-hashtable test-mono-handle	\
		 test-path \
		 test-mono-callspec test-mono-string test-mono-embed test-embed-invoke

TESTS = test-sgen-qsort test-memfuncs test-mono-linked-list-set test-conc-hashtable test-mono-handle \
	test-path \
	test-mono-callspec test-mono-string test-mono-embed test-embed-invoke

if ENABLE_LLVM
check_PROGRAMS += test-llvm-engine test-llvm-reloc-widths test-llvm-ehframe test-llvm-ehtable test-llvm-class-init
TESTS += test-llvm-engine test-llvm-reloc-widths test-llvm-ehframe test-llvm-ehtable test-llvm-class-init
endif

AM_TESTS_ENVIRONMENT = export MONO_PATH=$(mcs_topdir)/class/lib/build;

test-local: $(check_PROGRAMS)

.NOTPARALLEL:

check-local:
	if [ -e test-suite.log ]; then \
		if grep -q "# FAIL:  0\|tests passed" test-suite.log; then successbool=True && failures=0; else successbool=False && failures=1; fi; \
		echo "<?xml version='1.0' encoding='utf-8'?><test-results failures='$$failures' total='1' not-run='0' name='unit-tests.dummy' date='$$(date +%F)' time='$$(date +%T)'><test-suite name='MonoTests.unit-tests' success='$$successbool' time='0'><results><test-case name='MonoTests.unit-tests.100percentsuccess' executed='True' success='$$successbool' time='0'>" > TestResult-unit-tests.xml; \
		if [ $$failures -ne 0 ]; then echo "<failure><message>"'<![CDATA[' >> TestResult-unit-tests.xml && cat test-suite.log >> TestResult-unit-tests.xml && echo "]]></message><stack-trace></stack-trace></failure>" >> TestResult-unit-tests.xml; fi; \
		echo "</test-case></results></test-suite></test-results>" >> TestResult-unit-tests.xml; \
	fi;

clean-local:
	rm -f callspec.exe callspec.pdb 
	rm -f test-embed.exe test-embed.pdb
	rm -f test-embed-invoke-cs.exe test-embed-invoke-cs.pdb

%.exe: %.cs
	$(MCS) -r:$(CLASS)/System.dll -r:$(CLASS)/System.Xml.dll -r:$(CLASS)/System.Core.dll -out:$@ $<

test-embed.exe: $(top_srcdir)/samples/embed/test.cs
	$(MCS) -r:$(CLASS)/System.dll -r:$(CLASS)/System.Xml.dll -r:$(CLASS)/System.Core.dll -out:$@ $<

test-embed-invoke-cs.exe: $(top_srcdir)/samples/embed/invoke.cs
	$(MCS) -r:$(CLASS)/System.dll -r:$(CLASS)/System.Xml.dll -r:$(CLASS)/System.Core.dll  -out:$@ $<

endif !HOST_WIN32
endif !CROSS_COMPILE
