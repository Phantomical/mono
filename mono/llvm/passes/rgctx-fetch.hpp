/**
 * \file
 * \brief Putting a load of a generic-context slot in front of the call that fills it.
 */

#ifndef MONO_LLVM_PASSES_RGCTX_FETCH_HPP
#define MONO_LLVM_PASSES_RGCTX_FETCH_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// Marks a declaration whose calls fill a generic-context slot. This pass
/// rewrites the sites of such a declaration and nothing else.
constexpr llvm::StringRef rgctx_fetch_attribute = "mono-rgctx-fetch";

/// The attribute each fetch site carries, as `key=value` pairs: the operand the
/// context arrives in, and the byte offsets that reach the slot from it. Only
/// the translator writes it.
constexpr llvm::StringRef rgctx_walk_attribute = "mono-rgctx-walk";

/// The operand bundle each fetch site carries: the icall that actually fills
/// the slot the site names. The site itself takes the context as a pointer;
/// this pass converts to what the bundled icall takes when it lowers the
/// fetch. A bundle rather than an attribute, so a rename or a merge of that
/// icall's declaration elsewhere in the module carries over automatically.
constexpr llvm::StringRef rgctx_fill_bundle = "mono-rgctx-fill";

/// Reads each fetch site's slot before the call, and takes the call only when
/// the slot is still empty.
///
/// The guard is a diamond, so run this behind the passes that read the call as
/// one instruction. Run it before the tier's codegen and after its last
/// simplification.
class RgctxFetchPass : public llvm::PassInfoMixin<RgctxFetchPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m,
	                             llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif
