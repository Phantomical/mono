/**
 * \file
 * \brief Finds the class-init barriers a tier-1 body no longer needs.
 *
 * A cctor barrier is a two-part shape - a branch testing `vtable->initialized`
 * and, on the arm where it is clear, a call to the `mono_generic_class_init`
 * trigger. The translator tags both halves (`mono.class-init-check` on the
 * branch, `mono.class-init` on the call); see translator-call.cpp.
 *
 * A trigger call is redundant when, on every path that reaches it, the runtime
 * has already been asked to initialize that same class. This analysis reports
 * the calls for which a single dominating fact establishes that:
 *
 *   - an earlier trigger call for the same class, or
 *   - an earlier check that found `initialized` already set (its "yes" edge).
 *
 * Both facts are safe to lean on for slightly different reasons, and neither is
 * the tempting-but-false "the byte is now 1":
 *
 *   - After a trigger call returns, either the cctor has run, or the calling
 *     thread is the one already running it (mono_runtime_class_init_full ()
 *     reports success in that case with the byte still 0). A second call is a
 *     no-op under both, so the *call*, not the byte, is what licenses dropping
 *     the later one.
 *   - `initialized` is monotonic: once the runtime sets it, it stays set for
 *     the life of the vtable. So a check that observed it non-zero stays true
 *     down every path leaving that edge.
 *
 * Same class means same vtable, which is read out of the IR - the trigger's
 * argument, and the address the check loads - not out of the metadata's class
 * name, which is for humans and can repeat across assemblies. The tags decide
 * *which* instructions to look at; the operands decide what they mean. Anything
 * that does not decode to the expected shape is skipped, so an unrecognized
 * barrier costs a missed elision, never a wrong one.
 */

#ifndef MONO_MINI_LLVM_PASSES_ELIDE_CLASS_INIT_HPP
#define MONO_MINI_LLVM_PASSES_ELIDE_CLASS_INIT_HPP

/*
 * PassManager.h uses PIC as an identifier, and libtool passes -DPIC (as does
 * mono-tls.h when we are built as PIE), so the macro has to go before any LLVM
 * header - same dance as the other passes here.
 */
#ifdef PIC
#undef PIC
#endif

#include <cstdint>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/PassManager.h>

namespace llvm {
class CallBase;
class DominatorTree;
class Function;
class Instruction;
} // namespace llvm

namespace mono {

/* What made a trigger call redundant. */
enum class ClassInitFactKind {
	/* An earlier `mono_generic_class_init` call for the same class. */
	PriorCall,
	/* The "already initialized" edge of an earlier check for the same class. */
	InitializedEdge,
};

struct RedundantClassInit {
	/* The trigger call that need not run. */
	llvm::CallBase *call;
	/* The dominating fact: a trigger call, or the check branch it came from. */
	llvm::Instruction *fact;
	ClassInitFactKind kind;
	/* The MonoVTable the barrier is for, as it appears in the IR. */
	uint64_t vtable;
};

/*
 * The redundant trigger calls in F, in program order. DT must be F's dominator
 * tree.
 */
llvm::SmallVector<RedundantClassInit, 4>
find_redundant_class_inits (llvm::Function &f, const llvm::DominatorTree &dt);

/*
 * The same thing as a cached function analysis, for a pipeline consumer.
 */
class ClassInitElisionAnalysis : public llvm::AnalysisInfoMixin<ClassInitElisionAnalysis> {
public:
	class Result {
	public:
		explicit Result (llvm::SmallVector<RedundantClassInit, 4> redundant)
		    : redundant_ (std::move (redundant))
		{
		}

		llvm::ArrayRef<RedundantClassInit> redundant () const { return redundant_; }

		/* The reason CALL is redundant, or null if it is not. */
		const RedundantClassInit *lookup (const llvm::CallBase *call) const;

		bool invalidate (llvm::Function &f, const llvm::PreservedAnalyses &pa,
		                 llvm::FunctionAnalysisManager::Invalidator &inv);

	private:
		llvm::SmallVector<RedundantClassInit, 4> redundant_;
	};

	Result run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

private:
	friend llvm::AnalysisInfoMixin<ClassInitElisionAnalysis>;
	static llvm::AnalysisKey Key;
};

} // namespace mono

#endif /* MONO_MINI_LLVM_PASSES_ELIDE_CLASS_INIT_HPP */
