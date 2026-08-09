#ifndef MONO_LLVM_RUNTIME_HELPERS_HPP
#define MONO_LLVM_RUNTIME_HELPERS_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/TargetParser/Triple.h>
#include <vector>

namespace mono
{

/// A builtin function that we provide to the runtime.
///
/// This includes things like compiler-rt, but also some mono functions and
/// intrinsics like memset.
struct MonoBuiltin
{
    llvm::StringRef name;
    void* address;

    static std::vector<MonoBuiltin> get_platform_builtins(const llvm::Triple& triple);
};

}

#endif
