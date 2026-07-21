ACLOCAL_AMFLAGS = -I m4

SUBDIRS = @MONO_SUBDIRS@
noinst_SUBDIRS = @MONO_NOINST_SUBDIRS@
DIST_SUBDIRS = $(SUBDIRS) m4

if !ENABLE_NETCORE

all: $(update_submodules)

update_submodules:
	@cd $(srcdir) && scripts/update_submodules.sh

.PHONY: update_submodules compile-commands

if ENABLE_COMPILE_COMMANDS
# Merge the per-compile event files dropped by the bearcc/bearcxx wrappers into
# compile_commands.json.  Hooked to all-local rather than to `all:` so it runs
# after all-recursive has descended into SUBDIRS -- prerequisites of `all:` are
# unordered and would race the build under -j.
#
# citnames re-filters the entire database on every --append, so stale entries
# for deleted sources are dropped as a side effect.  Consumed event files are
# removed so the directory does not grow without bound.
all-local: compile-commands

compile-commands:
	@events=`find $(BEAR_EVENTS_DIR) -name 'e-*.json' 2>/dev/null`; \
	if test -z "$$events"; then \
		echo "  GEN      compile_commands.json (no new events, up to date)"; \
		exit 0; \
	fi; \
	echo "  GEN      compile_commands.json"; \
	merged=$(BEAR_EVENTS_DIR)/merged.$$$$.json; \
	cat $$events > $$merged || exit 1; \
	append=; test -f $(top_builddir)/compile_commands.json && append=--append; \
	if $(BEAR) citnames --input $$merged \
			--output $(top_builddir)/compile_commands.json \
			--config $(top_builddir)/bear-citnames.json $$append; then \
		rm -f $$events $$merged; \
	else \
		rm -f $$merged; \
		echo "  WARNING  bear citnames failed; compile_commands.json unchanged" >&2; \
	fi
endif

EXTRA_DIST= \
	README.md 		\
	LICENSE 		\
	autogen.sh 		\
	mkinstalldirs 		\
	mono-uninstalled.pc.in 	\
	winconfig.h 		\
	code_of_conduct.md 	\
	external		\
	mcs/class/referencesource

DISTCHECK_CONFIGURE_FLAGS = EXTERNAL_RUNTIME=false

# Distribute the 'mcs' tree too
GIT_DIR ?= $(srcdir)/.git
dist-hook:
	test -d $(distdir)/mcs || mkdir $(distdir)/mcs
	d=`cd $(distdir)/mcs && pwd`; cd $(mcs_topdir) && $(MAKE) distdir=$$d dist-recursive
	rm -rf `find $(top_distdir)/external -path '*\.git'`
	rm -rf `find $(top_distdir)/external -path '*\.libs'`
	rm -rf `find $(top_distdir)/external -path '*\.deps'`
	rm -f `find $(top_distdir)/external -path '*\.o'`
	rm -f `find $(top_distdir)/external -path '*\.so'`
	rm -f `find $(top_distdir)/external -path '*\.lo'`
	rm -f `find $(top_distdir)/external -path '*\.Plo'`
	rm -f `find $(top_distdir)/external -name '\.dirstamp'`
	rm -f `find $(top_distdir)/external -path '*\.exe' -not -path '*/roslyn-binaries/*'`
	rm -f `find $(top_distdir)/external -path '*\.dll' -not -path '*/binary-reference-assemblies/*' -not -path '*/roslyn-binaries/*' -not -path '*/helix-binaries/*'`
	rm -rf "$(top_distdir)/external/linker/test"
	rm -rf "$(top_distdir)/external/llvm-project/lldb/test"
	rm -rf "$(top_distdir)/external/llvm-project/libcxx/test"
	rm -rf "$(top_distdir)/external/llvm-project/clang/test"

pkgconfigdir = $(libdir)/pkgconfig
noinst_DATA = mono-uninstalled.pc
DISTCLEANFILES= mono-uninstalled.pc

# building with monolite
.PHONY: get-monolite-latest 
get-monolite-latest:
	$(MAKE) -C $(mcs_topdir)/class get-monolite-latest

if BITCODE
BITCODE_CHECK=yes
endif

if DEFAULT_TESTS
CI_TEST_SCRIPT=$(srcdir)/scripts/ci/run-test-default.sh
else
CI_TEST_SCRIPT=$(srcdir)/scripts/ci/run-test-$(TEST_PROFILE).sh
endif

.PHONY: check-ci
check-ci:
	MONO_LLVMONLY=$(BITCODE_CHECK) $(CI_TEST_SCRIPT)

.PHONY: validate do-build-mono-mcs mcs-do-clean mcs-do-tests
validate: do-build-mono-mcs
	$(MAKE) mcs-do-tests
do-build-mono-mcs: mcs-do-clean
	$(MAKE) all
mcs-do-clean:
	cd runtime && $(MAKE) clean-local
	cd mono/tests && $(MAKE) clean
mcs-do-tests:
	cd runtime && $(MAKE) check-local
	cd mono/tests && $(MAKE) check

.PHONY: compiler-tests mcs-do-compiler-tests
compiler-tests:
	$(MAKE) test_select='TEST_SUBDIRS="tests errors"' validate
mcs-do-compiler-tests:
	$(MAKE) test_select='TEST_SUBDIRS="tests errors"' mcs-do-tests

.PHONY: bootstrap-world
bootstrap-world: compiler-tests
	$(MAKE) install

install:
	for mydir in $(filter-out $(noinst_SUBDIRS),$(SUBDIRS)); do \
	  (cd $${mydir} && ${MAKE} install) \
	done

# Update llvm version in configure.ac to the output of $LLVM_DIR/bin/llvm-config --version
update-llvm-version:
	if test "x$$LLVM_DIR" = "x"; then echo "Set the make variable LLVM_DIR to the directory containing the LLVM installation."; exit 1; fi
	REV=`$(LLVM_DIR)/bin/llvm-config --version` && sed -e "s,expected_llvm_version=.*,expected_llvm_version=\"$$REV\"," < configure.ac > tmp && mv tmp configure.ac && echo "Version set to $$REV."

endif
