
# The mono SDK used to build LLVM from the vendored `external/llvm-project`
# submodule (a Mono-patched LLVM 6.0.1 fork) and to download prebuilt tarballs
# keyed by that submodule's git HEAD. That submodule has been removed: the LLVM
# back end now builds against an externally supplied, unmodified upstream LLVM
# (14 or newer) selected with `configure --with-llvm=<prefix>`.
#
# These targets are reachable only from upstream Mono's Jenkins driver
# (scripts/ci/run-jenkins.sh, CI_TAGS=sdks-llvm and the ios/android/mac/wasm
# cross builds), which this fork does not run -- Unity CI is .yamato/*.yml plus
# external/buildscripts, and neither references sdks/builds or llvm-project.
#
# The target *names* are kept because android.mk / mac.mk / wasm.mk name them as
# prerequisites (provision-llvm-llvm64, provision-llvm-llvmwin64, ...) and would
# fail to parse without them. They now fail with an explicit message instead of
# silently building nothing or downloading a bogus URL. If the mono SDK cross
# builds are ever revived, they need to acquire an upstream LLVM here.

LLVM_REMOVED_MESSAGE = the external/llvm-project submodule was removed; the mono SDK LLVM builds need an externally supplied upstream LLVM 14+ (see llvm/Makefile.am and configure --with-llvm)

##
# Parameters
#  $(1): version
#  $(2): target
define LLVMProvisionTemplate

.PHONY: download-$(1)-$(2)
download-$(1)-$(2):
	$$(error $(1)-$(2): $$(LLVM_REMOVED_MESSAGE))

.PHONY: provision-$(1)-$(2)
provision-$(1)-$(2):
	$$(error $(1)-$(2): $$(LLVM_REMOVED_MESSAGE))

.PHONY: archive-$(1)-$(2)
archive-$(1)-$(2):
	$$(error $(1)-$(2): $$(LLVM_REMOVED_MESSAGE))

endef

$(eval $(call LLVMProvisionTemplate,llvm,llvm64))
$(eval $(call LLVMProvisionTemplate,llvm,llvmwin64))
ifeq ($(UNAME),Windows)
$(eval $(call LLVMProvisionTemplate,llvm,llvmwin64-msvc))
endif

##
# Parameters
#  $(1): target
define LLVMTemplate

.PHONY: setup-llvm-$(1)
setup-llvm-$(1):
	mkdir -p $$(TOP)/sdks/out/llvm-$(1)

.PHONY: package-llvm-$(1)
package-llvm-$(1):
	$$(error llvm-$(1): $$(LLVM_REMOVED_MESSAGE))

.PHONY: clean-llvm-$(1)
clean-llvm-$(1)::
	$$(RM) -r $$(TOP)/sdks/builds/llvm-$(1) $$(TOP)/sdks/out/llvm-$(1)

endef

$(eval $(call LLVMTemplate,llvm64))

ifneq ($(MXE_PREFIX),)
$(eval $(call LLVMTemplate,llvmwin64))
endif

##
# The MSVC path drives msvc/mono.sln, which builds LLVM from
# MONO_INTERNAL_LLVM_SOURCE_DIR. That variable is now mandatory (see
# msvc/mono.external.targets); there is no in-tree default any more.
#
# Parameters
#  $(1): target
#  $(2): arch
define LLVMMsvcTemplate

.PHONY: setup-llvm-$(1)
setup-llvm-$(1):
	mkdir -p $$(TOP)/sdks/out/llvm-$(1)

.PHONY: package-llvm-$(1)
package-llvm-$(1): setup-llvm-$(1)
	$$(TOP)/llvm/build_llvm_msbuild.sh "build" "$(2)" "release" "$$(TOP)/msvc/" "$$(TOP)/sdks/builds/llvm-$(1)" "$$(TOP)/sdks/out/llvm-$(1)"

.PHONY: clean-llvm-$(1)
clean-llvm-$(1):
	$$(TOP)/llvm/build_llvm_msbuild.sh "clean" "$(2)" "release" "$$(TOP)/msvc/" "$$(TOP)/sdks/builds/llvm-$(1)" "$$(TOP)/sdks/out/llvm-$(1)"

endef

ifeq ($(UNAME),Windows)
$(eval $(call LLVMMsvcTemplate,llvmwin64-msvc,x86_64))
endif
