#!/usr/bin/env bash
#
# Self-contained build for the LLVM 18 ORCv2 JIT de-risk spike.
# Uses ONLY the system LLVM 18 via llvm-config-18. Installs nothing.
#
set -euo pipefail
cd "$(dirname "$0")"

LLVM_CONFIG="${LLVM_CONFIG:-llvm-config-18}"

echo "Using $($LLVM_CONFIG --version) from $($LLVM_CONFIG --prefix)"

CXX="${CXX:-clang++}"

# llvm-config drives the bulk of the flags (-std=c++17, -fno-exceptions,
# -D__STDC_*_MACROS, include dir). NOTE: --cxxflags does NOT emit -fno-rtti,
# yet libLLVM-18 on this box was itself built -fno-rtti (verified: `nm -DC`
# shows no `typeinfo for llvm::`). For this spike it is harmless (we subclass
# nothing), but we append -fno-rtti explicitly to match the library's ABI --
# step 3 subclasses polymorphic LLVM classes (SectionMemoryManager,
# ObjectLinkingLayer::Plugin, ...) where an RTTI on/off mismatch is the
# classic silent ABI break. So set it now and keep it consistent.
CXXFLAGS="$($LLVM_CONFIG --cxxflags) -fno-rtti"
LDFLAGS="$($LLVM_CONFIG --ldflags)"
# Shared mode on this box -> a single -lLLVM-18. --system-libs is empty here
# but included for portability to a static-LLVM machine.
LIBS="$($LLVM_CONFIG --libs orcjit native) $($LLVM_CONFIG --system-libs)"

# -rdynamic: export driver symbols (host_triple) into the dynamic symbol
# table so ORCv2's process search generator can dlsym them at JIT time.
# -Wl,-rpath: find libLLVM-18.so at runtime without touching LD_LIBRARY_PATH.
RPATH="-Wl,-rpath,$($LLVM_CONFIG --libdir)"

set -x
$CXX $CXXFLAGS engine_spike.cpp -o engine_spike \
    $LDFLAGS $LIBS $RPATH -rdynamic
