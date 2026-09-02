/**
 * \file
 * \brief The callees tier 2 weighs before folding them in.
 */

#ifndef MONO_LLVM_RUNTIME_PROFILE_INLINES_HPP
#define MONO_LLVM_RUNTIME_PROFILE_INLINES_HPP

#include "inline-scope.hpp"
#include "passes/top-down-inline.hpp"
#include "translate.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include <utility>
#include <vector>

typedef struct _MonoDomain MonoDomain;

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/// Answers the tier-2 inliner's questions about managed methods, and translates
/// the bodies it asks for.
///
/// It materializes from inside the IR pipeline, which is past the naming and
/// the resolution a compile does for the root. So it names and resolves what
/// each body it adds refers to, appending to the same module_symbols the link
/// is given. A body whose own callees will not resolve is taken back off and the
/// site keeps its call. A candidate on a path the root never runs must not
/// decide what the root compiles to.
class ProfileInliner final : public InlineCandidates {
public:
	/// scope must already name the root. externals, types and module_symbols are
	/// the compile's own, and all three have to outlive this.
	///
	/// types is shared with the module the root was translated into. A named
	/// struct type belongs to the LLVMContext rather than to one module, so a
	/// candidate built against a cache of its own would name a second type of
	/// the same shape, and the link would keep both.
	ProfileInliner (const TranslationTarget &target,
	                std::vector<ExternalSymbol> &externals, ModuleTypes &types,
	                InlineScope &scope,
	                std::vector<std::pair<llvm::StringRef, void *>> &module_symbols)
	    : target_ (target), externals_ (externals), types_ (types), scope_ (scope),
	      module_symbols_ (module_symbols)
	{
	}

	llvm::Function *materialize (llvm::Function &decl, llvm::Module &into) override;
	llvm::ArrayRef<uint8_t> profile_for (llvm::Function &decl) override;
	void folded (llvm::Function &caller, llvm::Function &callee) override;
	void declined (llvm::Function &caller, llvm::Function &callee,
	               const llvm::InlineCost &cost, uint64_t count) override;
	unsigned depth_limit () const override;
	unsigned round_limit () const override;

private:
	const TranslationTarget &target_;
	std::vector<ExternalSymbol> &externals_;
	ModuleTypes &types_;
	InlineScope &scope_;
	std::vector<std::pair<llvm::StringRef, void *>> &module_symbols_;

	/// Owns the bytes the last profile_for () call returned; a second call
	/// overwrites them.
	std::vector<uint8_t> profile_scratch_;

	llvm::Error bind_and_resolve (llvm::Module &module, size_t from, size_t to);
};

} // namespace mono

#endif
