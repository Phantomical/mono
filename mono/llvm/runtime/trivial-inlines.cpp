#include "runtime-error.hpp"

#include "trivial-inlines.hpp"

#include "inline-scope.hpp"
#include "method-symbols.hpp"
#include "method-to-llvm.hpp"
#include "minimal-compile.hpp"
#include "options.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <optional>

#include "mini.h"
#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/tabledefs.h"

using namespace llvm;

namespace mono {

namespace {

struct Shape {
	/// The method the single call site names, null when the body calls nothing.
	MonoMethod *forwards_to = nullptr;
};

/// Whether an opcode keeps the body it is in out of the pre-pass.
///
/// A body holding none of these is left to is_small_and_clause_free (), which is
/// what bounds the cost. Nothing on this list is here for cost: the opcodes it
/// permits already emit a branch and a throw, because ldfld reaches its field
/// through emit_null_check () (method-to-llvm/fields.cpp), and box allocates.
/// What the list protects is the one-line shape shape_of () walks, and the frame
/// the fold takes away.
bool
declines_a_fold (MonoOpcodeEnum op)
{
	switch (op) {
	/*
	 * Control flow. shape_of () walks the IL forward once and reads a
	 * terminator as the end of the body, so a second edge is a shape it cannot
	 * describe. br and br.s are here as well: the fallthrough case is the
	 * caller's, which reads the displacement.
	 */
	case MONO_CEE_BR:
	case MONO_CEE_BR_S:
	case MONO_CEE_BRFALSE:
	case MONO_CEE_BRFALSE_S:
	case MONO_CEE_BRTRUE:
	case MONO_CEE_BRTRUE_S:
	case MONO_CEE_BEQ:
	case MONO_CEE_BEQ_S:
	case MONO_CEE_BGE:
	case MONO_CEE_BGE_S:
	case MONO_CEE_BGE_UN:
	case MONO_CEE_BGE_UN_S:
	case MONO_CEE_BGT:
	case MONO_CEE_BGT_S:
	case MONO_CEE_BGT_UN:
	case MONO_CEE_BGT_UN_S:
	case MONO_CEE_BLE:
	case MONO_CEE_BLE_S:
	case MONO_CEE_BLE_UN:
	case MONO_CEE_BLE_UN_S:
	case MONO_CEE_BLT:
	case MONO_CEE_BLT_S:
	case MONO_CEE_BLT_UN:
	case MONO_CEE_BLT_UN_S:
	case MONO_CEE_BNE_UN:
	case MONO_CEE_BNE_UN_S:
	case MONO_CEE_SWITCH:
	case MONO_CEE_LEAVE:
	case MONO_CEE_LEAVE_S:
	case MONO_CEE_ENDFINALLY:
	case MONO_CEE_ENDFILTER:
	case MONO_CEE_RETHROW:
	case MONO_CEE_JMP:

	// A call the shape test cannot name a target for, so the one-call rule
	// cannot hold it and may_read_the_callers_frame () has nothing to follow.
	case MONO_CEE_CALLI:
	// A tail call hands the frame over, and the folded copy is standing in that
	// frame.
	case MONO_CEE_TAIL_:

	/*
	 * The frame. Each of these describes the frame the body runs in, which the
	 * fold replaces with the caller's.
	 */
	// Reads the frame it was called from, which is the hazard
	// may_read_the_callers_frame () exists for.
	case MONO_CEE_ARGLIST:
	// emit_user_break () calls a helper that walks the stack for a managed
	// frame to report the break against, and asserts that it finds one.
	case MONO_CEE_BREAK:
	// A typedref is frame-shaped.
	case MONO_CEE_MKREFANY:
	case MONO_CEE_REFANYVAL:
	case MONO_CEE_REFANYTYPE:
	// An alloca folded into a caller lives as long as that caller's frame.
	case MONO_CEE_LOCALLOC:
	// Publishes a thunk and asks for a compile that lands at tier 0. That is a
	// tiering side effect rather than a value.
	case MONO_CEE_LDFTN:
	case MONO_CEE_LDVIRTFTN:
		return true;
	default:
		return false;
	}
}

bool
enters_a_method (MonoOpcodeEnum op)
{
	return op == MONO_CEE_CALL || op == MONO_CEE_CALLVIRT || op == MONO_CEE_NEWOBJ;
}

bool
branches_to_the_next (const unsigned char *code, MonoOpcodeEnum op, size_t operand)
{
	// A displacement is counted from the instruction behind the branch, so zero
	// is that instruction.
	if (op == MONO_CEE_BR_S)
		return (int8_t) code[operand] == 0;
	if (op == MONO_CEE_BR)
		return (int32_t) il_read_u32 (code + operand) == 0;

	return false;
}

/// Returns the method's shape when its IL is one straight line. The line holds
/// at most one call, declines_a_fold () names what it may not hold, and its
/// terminator is the last IL byte. Returns nullopt otherwise.
///
/// These are the shapes worth folding in without weighing them:
///
///   ldc.i4.1                       ret a constant
///   ret
///
///   ldarg.0  ldfld y  ldfld z      ret a chain of fields
///   ret
///
///   ldarg.0  ldarg.1  stfld x      write one
///   ret
///
///   ldarg.0  ldfld y  ldarg.1      forward to one other method
///   call  F
///   ret
///
///   ldarg.1  newobj X::.ctor       throw
///   throw
///
///   ldarg.1  newobj Y::.ctor       make an object and return it
///   ret
///
///   ldarg.0  ldarg.1  sizeof T     step a pointer by an element
///   conv.i  mul  add
///   ret
///
/// One line is what the shape test is for. It bounds the walk below, and it
/// leaves the method with one terminator. What the line costs is the size
/// limit's question rather than this one's.
std::optional<Shape>
shape_of (MonoMethod *method, MonoMethodHeader *header)
{
	const unsigned char *code = header->code;
	size_t size = header->code_size;
	Shape shape;
	size_t at = 0;

	while (at < size) {
		const unsigned char *cursor = code + at;
		MonoOpcodeEnum op = mono_opcode_value (&cursor, code + size);

		if (op == MonoOpcodeEnum_Invalid)
			return std::nullopt;

		size_t operand = (size_t) (cursor - code) + 1;
		std::optional<size_t> width = il_operand_size (op);

		if (!width || operand + *width > size)
			return std::nullopt;

		size_t next = operand + *width;

		if (op == MONO_CEE_RET || op == MONO_CEE_THROW) {
			if (next != size)
				return std::nullopt;

			return shape;
		}

		if (enters_a_method (op)) {
			if (shape.forwards_to != nullptr)
				return std::nullopt;

			shape.forwards_to = il_call_target (method, il_read_u32 (code + operand));
			if (shape.forwards_to == nullptr)
				return std::nullopt;
		} else if (declines_a_fold (op)
		           // A C# compiler ends a value-returning method with
		           // stloc.0, a branch to the next instruction, ldloc.0,
		           // then ret. Letting that one branch through as a
		           // fallthrough keeps such a getter or forwarder on one
		           // line.
		           && !branches_to_the_next (code, op, operand)) {
			return std::nullopt;
		}

		at = next;
	}

	// The IL ran off its own end without reaching a terminator.
	return std::nullopt;
}

// How far the two walks below follow a chain of forwarders. Longer than any
// chain worth following, and each link costs a header. Both use the same bound,
// which is what lets each one rely on what the other does with a chain that
// outruns it.
constexpr int max_links = 8;

/// Whether target reaches itself through the forwarder chain shape_of ()
/// describes.
///
/// A recursive body is one no inliner can fold the call out of, so the pre-pass
/// declines it however small it is. Direct recursion and a cycle through other
/// forwarders read the same here.
bool
forwards_into_a_cycle (MonoMethod *target, MonoDomain *domain)
{
	SmallPtrSet<MonoMethod *, max_links> seen;

	for (int link = 0; link < max_links && target != nullptr; ++link) {
		if (!seen.insert (target).second)
			return true;

		if (implemented_outside_il (target))
			return false;

		ERROR_DECL (metadata_error);
		MinimalCompile cfg (target, domain, metadata_error);
		MonoMethodHeader *header = cfg.get ()->header;

		if (header == nullptr) {
			mono_error_cleanup (metadata_error);
			return false;
		}

		// A body the shape test declines ends the chain. It keeps its own call
		// sites, so a cycle behind it is one the pre-pass never folds through.
		if (header->num_clauses != 0)
			return false;

		std::optional<Shape> shape = shape_of (target, header);

		if (!shape)
			return false;

		target = shape->forwards_to;
	}

	/*
	 * A chain that reaches here is at least max_links long, so the walk
	 * may_read_the_callers_frame () starts one link further along runs out too
	 * and refuses the body. Answering "no cycle" costs nothing.
	 */
	return false;
}

/// Whether target, or something it forwards to in turn, can read the frame it
/// was called from.
///
/// Follows the forwarder chain to the internal call at its end, which is what
/// reads_the_callers_frame () (inline-scope.hpp) decides. A chain this walk does
/// not reach the end of answers yes.
///
/// NoInlining on a link is not read, for the reason loses_its_frame_safely ()
/// gives.
bool
may_read_the_callers_frame (MonoMethod *target, MonoDomain *domain)
{
	for (int link = 0; link < max_links; ++link) {
		if (target == nullptr)
			return false;

		// A body with no IL is where the chain stops. It keeps no frame of its
		// own, so what it reports comes from the frame that called it.
		if (implemented_outside_il (target))
			return reads_the_callers_frame (target);

		ERROR_DECL (metadata_error);
		MinimalCompile cfg (target, domain, metadata_error);
		MonoMethodHeader *header = cfg.get ()->header;

		if (header == nullptr) {
			mono_error_cleanup (metadata_error);
			return false;
		}

		// A body with clauses is not a forwarder, and neither is one the shape
		// test declines. Either way it keeps a frame of its own, which is where
		// the chain stops. No size limit here: what matters is the shape, and a
		// straight line to one call can be longer than anything the pre-pass
		// folds.
		if (header->num_clauses != 0)
			return false;

		std::optional<Shape> shape = shape_of (target, header);

		if (!shape)
			return false;

		target = shape->forwards_to;
	}

	return true;
}

/// A copy belongs to the one compile that asked for it. Another body in the
/// module keeps the declaration and reaches the published entry, until its own
/// compile folds a copy of its own.
void
redirect_calls (Function &caller, Function &from, Function &to)
{
	for (Instruction &i : instructions (caller)) {
		auto *site = dyn_cast<CallBase> (&i);

		if (site != nullptr && site->getCalledFunction () == &from)
			site->setCalledFunction (&to);
	}
}

void
trace_inline (MonoMethod *callee, MonoMethod *caller)
{
	char *host = mono_method_full_name (caller, TRUE);
	char *what = mono_method_full_name (callee, TRUE);

	MONO_LOCK (jit_trace_mutex ())
	{
		fprintf (stderr, "[llvm-jit] folding %s into %s\n", what, host);
	}
	g_free (what);
	g_free (host);
}

} // namespace

void
materialize_trivial_callees (Module &module, MonoDomain *domain, MonoMethod *root,
                             Function &body, std::vector<ExternalSymbol> &externals,
                             ModuleTypes &types, InlineScope &scope,
                             ResolveExternals resolve)
{
	uint32_t limit = trivial_inline_il_limit ();

	// A breakpoint is armed on a method, and a folded copy carries none of the
	// method's sequence points.
	if (limit == 0 || mini_get_debug_options ()->gen_sdb_seq_points)
		return;

	// A body the module now holds, and the method it belongs to. A body reached
	// through another one is folded into that one first, so the pair is what a
	// trace has to name.
	struct Candidate {
		MonoMethod *method;
		llvm::Function *body;

		/// Folds between this body and root. Zero is root itself.
		unsigned depth;
	};

	SmallVector<Candidate, 8> pending { { root, &body, 0 } };

	// The methods whose copy the resolution below refused. scope.folded keeps an
	// entry naming a copy that has gone. A later site for one of these asks for
	// a copy again and meets the same failure.
	SmallPtrSet<MonoMethod *, 4> unresolved;

	// The budget bounds what the loop translates rather than how far it walks.
	// A body it can no longer fold into still has sites to move onto the copies
	// this root holds already.
	/*
	 * Least deep first. The worklist is drained from the front, so every body
	 * one remove from root is translated before any body behind it, and a budget
	 * that runs out drops the deepest candidates rather than whichever chain
	 * the walk happened to go down.
	 */
	for (size_t next = 0; next < pending.size (); ++next) {
		auto [into, caller, depth] = pending[next];
		SmallVector<Function *, 8> called;

		for (Instruction &i : instructions (*caller)) {
			auto *site = dyn_cast<CallBase> (&i);
			Function *decl =
				site != nullptr ? site->getCalledFunction () : nullptr;

			if (decl != nullptr && decl->isDeclaration ()
			    && !is_contained (called, decl))
				called.push_back (decl);
		}

		for (Function *decl : called) {
			MonoMethod *callee = marked_method (*decl);

			if (callee == nullptr || unresolved.contains (callee))
				continue;

			/*
			 * A copy belongs to the root rather than to the caller that
			 * asked for it, so these sites reach the one that stands. This
			 * is ahead of the budget because a redirect translates nothing:
			 * a root that has spent its budget still moves its sites over.
			 */
			bool rebuild = already_folded (scope, callee);

			if (rebuild) {
				// root has a body rather than a copy, and a copy of root
				// folded back into root has no end.
				if (callee == scope.root)
					continue;

				Function *standing = folded_copy_in (scope, callee, module);

				/*
				 * A copy stands, so these sites reach it instead of the
				 * published entry. One that already reaches caller keeps
				 * its call: the two would fold into each other otherwise.
				 */
				if (standing != nullptr) {
					if (!copy_reaches (*standing, *caller)) {
						g_assert (standing->getFunctionType ()
						          == decl->getFunctionType ());
						redirect_calls (*caller, *decl, *standing);
					}

					continue;
				}

				/*
				 * No copy stands here, so fall through and build one. The
				 * pipeline erases a copy once it has folded every call to
				 * it, and a copy made for a candidate belongs to that
				 * candidate's module, so a root meets this on the ordinary
				 * path rather than a rare one.
				 */
			}

			// A rebuild is free, so a spent budget stops the new methods
			// below it rather than the whole scan.
			if (!rebuild && scope.budget.trivial == 0)
				continue;

			if (!may_fold (domain, callee))
				continue;

			ERROR_DECL (metadata_error);
			MinimalCompile cfg (callee, domain, metadata_error);
			MonoMethodHeader *header = cfg.get ()->header;

			if (header == nullptr) {
				mono_error_cleanup (metadata_error);
				continue;
			}

			if (!is_small_and_clause_free (header, limit))
				continue;

			std::optional<Shape> shape = shape_of (callee, header);

			if (!shape)
				continue;

			if (forwards_into_a_cycle (callee, domain))
				continue;

			if (shape->forwards_to != nullptr
			    && may_read_the_callers_frame (shape->forwards_to, domain))
				continue;

			size_t before = externals.size ();
			Function *copy =
				materialize_inline_copy (module, domain, callee, cfg.get (),
			                                 externals, types, scope,
			                                 Inliner::trivial);

			if (copy == nullptr)
				continue;

			/*
			 * A class the copy names may fail to load, and the program is owed
			 * that failure at the call rather than at root's entry. The call
			 * can sit inside a try whose catch is written for it. Taking the
			 * copy back off leaves the call on the callee's thunk, where the
			 * callee's own compile raises it. root then keeps the body it
			 * would have had, clauses and all.
			 */
			if (Error err = resolve (ArrayRef (externals).drop_front (before))) {
				consumeError (std::move (err));
				externals.resize (before);
				copy->eraseFromParent ();
				unresolved.insert (callee);
				continue;
			}

			if (is_jit_trace_enabled ())
				trace_inline (callee, into);

			// A shared body is entered with its context in a register and a
			// call to it is not, which is the one shape these two disagree
			// on. may_fold () refuses that callee, and a mismatch that got
			// through here calls the copy with the wrong arguments.
			g_assert (copy->getFunctionType () == decl->getFunctionType ());

			redirect_calls (*caller, *decl, *copy);

			// These shapes have nothing to weigh, so the pipeline folds them
			// rather than a cost model.
			copy->addFnAttr (Attribute::AlwaysInline);

			/*
			 * A rebuild is a body this root has already walked, so its own
			 * callees were weighed the first time. Walking it again reaches
			 * past what the first fold decided and spends the budget on
			 * methods that fold deeper rather than on the sites in hand.
			 *
			 * The depth bound is how far past root the loop follows a chain
			 * of forwarders. It drains least deep first, so the bound decides
			 * where the count left over goes rather than what the first folds
			 * are.
			 */
			if (!rebuild && depth + 1 < trivial_inline_depth_limit ())
				pending.push_back ({ callee, copy, depth + 1 });
		}
	}
}

} // namespace mono
