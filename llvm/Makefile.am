
# Mono no longer vendors an LLVM source tree. LLVM must be supplied externally
# via --with-llvm=<prefix> (LLVM 14 or newer, unmodified upstream); configure
# rejects --enable-llvm without it. All this directory does now is turn the
# external llvm-config into llvm_config.mk for mono/mini to include.

EXTRA_DIST=build_llvm_config.sh

if ENABLE_LLVM

if HAVE_STATIC_ZLIB
llvm_extra_libs = $(STATIC_ZLIB_PATH)
else
if HAVE_SYS_ZLIB
llvm_extra_libs=-lz
else
llvm_extra_libs=
endif
endif

all-local: llvm_config.mk

clean-local: clean-llvm-config

$(mono_build_root)/llvm/llvm_config.mk: $(top_srcdir)/llvm/Makefile.am
	$(top_srcdir)/llvm/build_llvm_config.sh "$(EXTERNAL_LLVM_CONFIG)" "$(LLVM_CODEGEN_LIBS)" "$(llvm_extra_libs)" | tee $@

llvm_config.mk: $(mono_build_root)/llvm/llvm_config.mk

clean-llvm-config:
	- rm -rf llvm_config.mk

else
all-local:

clean-local:
endif

# Override this so we don't try to re-copy llvm when we install mono
install:
