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
	/// module is the one the root was translated into, and scope must already
	/// name the root. externals, types and module_symbols are the compile's own,
	/// and all four have to outlive this.
	ProfileInliner (llvm::Module &module, const TranslationTarget &target,
	                std::vector<ExternalSymbol> &externals, ModuleTypes &types,
	                InlineScope &scope,
	                std::vector<std::pair<llvm::StringRef, void *>> &module_symbols)
	    : module_ (module), target_ (target), externals_ (externals), types_ (types),
	      scope_ (scope), module_symbols_ (module_symbols)
	{
	}

	llvm::Function *materialize (llvm::Function &decl) override;
	void folded (llvm::Function &caller, llvm::Function &callee) override;
	unsigned depth_limit () const override;

private:
	llvm::Module &module_;
	const TranslationTarget &target_;
	std::vector<ExternalSymbol> &externals_;
	ModuleTypes &types_;
	InlineScope &scope_;
	std::vector<std::pair<llvm::StringRef, void *>> &module_symbols_;

	/// Names and resolves what the externals recorded since \p from refer to.
	llvm::Error bind_and_resolve (size_t from);
};

} // namespace mono

#endif
