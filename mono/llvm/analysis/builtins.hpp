/**
 * \file
 * \brief What a builtin site's own operand already answers: a cast against a
 * known class, and a delegate against the method it was built over.
 *
 * Neither question belongs to `MonoBuiltinConstProp` alone. The tier-2 cost
 * model asks the cast question to weigh an inline, and the front end asks the
 * delegate question to mark and recognize the shapes it wrote.
 */

#ifndef MONO_LLVM_ANALYSIS_BUILTINS_HPP
#define MONO_LLVM_ANALYSIS_BUILTINS_HPP

#include <llvm/ADT/STLFunctionalExtras.h>

namespace llvm {
class CallBase;
class LoadInst;
class PHINode;
class Value;
} // namespace llvm

typedef struct _MonoClass MonoClass;
typedef struct _MonoMethod MonoMethod;

namespace mono {

class ConstantValues;

/// What a test of an operand against a class answers for every class the
/// operand can hold.
enum class CastAnswer { Unknown, Yes, No };

/// How a test against \p target comes out for an operand of class \p held.
///
/// \p exact says that held is the class the operand is. False makes it a bound:
/// the operand holds some class assignable to held, and an answer needs every
/// one of them to agree.
///
/// Unknown is always available and always right, so a shape this does not model
/// leaves the site alone.
CastAnswer cast_answer (MonoClass *target, MonoClass *held, bool exact);

/// Whether \p answer settles every one of \p phi's own incoming edges. Does
/// not require them to settle the same way.
bool isinst_settles_over_incoming (
	llvm::PHINode &phi, llvm::function_ref<CastAnswer (llvm::Value *)> answer);

/// Rebuilds an isinst answer one incoming edge of \p phi at a time, where
/// \p answer decides each edge on its own. Null where one edge is undecided.
llvm::Value *rebuild_isinst_over_incoming (
	llvm::PHINode &phi, llvm::function_ref<CastAnswer (llvm::Value *)> answer);

/// Marks \p load as a read of `MonoDelegate::method_ptr` that does not change.
///
/// `mono_delegate_ctor ()` sets the field once, before anything else can see
/// the delegate: to the bound method's entry, or to null for one that combines
/// others. Nothing writes it again - an `ldvirtftn` delegate never writes back
/// the override it later resolves, and `Combine` builds a new object rather
/// than mutating one. `invoke_impl` sits next to it in the same struct
/// and does not take this mark - a trampoline patches that field in place on
/// the delegate's first dispatch.
///
/// The write happens inside `mono_delegate_ctor ()`, opaque to this compile, so
/// there is no store to carry the matching tag the way
/// `mark_object_vtable_read ()` needs one. Two tagged reads of the same
/// pointer still equate to each other across whatever runs between them.
void mark_delegate_method_ptr_read (llvm::LoadInst *load);

/// What the IR says about the delegate arriving at a site.
struct DelegateTarget {
	/// The method named, or null where no arm named one or two arms disagreed.
	MonoMethod *method = nullptr;

	/// Whether every delegate that can arrive calls that method. False with a
	/// method means it is a candidate to compare against, not the answer.
	bool settled = false;
};

/// What \p receiver says about the delegate it holds.
///
/// Reads a value the translator marked through whatever phis, selects and field
/// copies stand between, which \p values is what settles.
DelegateTarget delegate_target_at (llvm::Value *receiver,
                                  const ConstantValues &values);

/// Whether \p site reads its callee out of the delegate it passes.
///
/// `delegate_invoke_callee ()` (`method-to-llvm/call.cpp`) writes every Invoke as
/// one shape, and this is that shape read back:
///
///     %impl   = load ptr, ptr (getelementptr i8, %d, invoke_impl)
///     %isnull = icmp eq ptr %impl, null
///     %callee = select i1 %isnull, %dispatch, %impl
///     call %callee (%d, ...)
///
/// The delegate the site passes has to be the object the load reads, which is
/// what separates this from any other call through a selected pointer.
///
/// Reading the shape rather than a mark on the call is what still recognizes a
/// site an inliner moved. Metadata does not survive a transform that builds a
/// new instruction, and inlining a body into a try does exactly that: it writes
/// each call again as an invoke of the caller's pad.
bool reads_callee_off_delegate (const llvm::CallBase &site);

} // namespace mono

#endif
