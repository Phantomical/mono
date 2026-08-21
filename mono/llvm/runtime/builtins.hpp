#ifndef MONO_LLVM_RUNTIME_HELPERS_HPP
#define MONO_LLVM_RUNTIME_HELPERS_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/TargetParser/Triple.h>
#include <vector>

namespace mono
{

/// A name and address pair the JIT registers as a symbol compiled code can
/// call.
///
/// get_platform_builtins () gathers two kinds: mono's own runtime helpers,
/// and compiler support routines - memset, integer-division helpers, and so
/// on. The second kind is whatever LLVM's runtime-libcalls table names that
/// this process already has loaded.
struct MonoBuiltin
{
    llvm::StringRef name;
    void* address;

    static std::vector<MonoBuiltin> get_platform_builtins(const llvm::Triple& triple);
};

}

#endif
