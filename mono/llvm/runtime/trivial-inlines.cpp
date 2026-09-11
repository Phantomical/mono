#include "runtime-error.hpp"

#include "trivial-inlines.hpp"

#include "inline-scope.hpp"
#include "method-symbols.hpp"
#include "method-to-llvm.hpp"
#include "minimal-compile.hpp"
#include "naming.hpp"
#include "options.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <optional>

#include "mini.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/tabledefs.h"

using namespace llvm;

namespace mono {

namespace {

struct Shape {
	/// Null when the body calls nothing.
	MonoMethod *call = nullptr;
};

/// Whether an opcode keeps its body out of the shape-test pre-pass. A body
/// holding none of these is left to is_small_and_clause_free (), which bounds
/// the cost.
bool
blocks_a_fold (MonoOpcodeEnum op)
{
	switch (op) {
	// match_trivial_shape () walks the IL once and stops at a terminator, so
	// any of these gives the body a second edge it cannot describe.
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

	// No target to check the one-call rule against.
	case MONO_CEE_CALLI:
	// Hands the frame to the callee; the folded copy stands in for it.
	case MONO_CEE_TAIL_:

	case MONO_CEE_ARGLIST:
	// emit_user_break () walks the stack for a managed frame to report
	// against, and asserts that it finds one.
	case MONO_CEE_BREAK:
	// Names a location in the frame it was made in.
	case MONO_CEE_MKREFANY:
	case MONO_CEE_REFANYVAL:
	case MONO_CEE_REFANYTYPE:
	// Would live as long as the caller's frame instead of the callee's.
	case MONO_CEE_LOCALLOC:
	// Publishes a thunk and asks for a compile that lands at tier 0, a
	// side effect rather than a value.
	case MONO_CEE_LDFTN:
	case MONO_CEE_LDVIRTFTN:
		return true;
	default:
		return false;
	}
}

bool
is_call_opcode (MonoOpcodeEnum op)
{
	return op == MONO_CEE_CALL || op == MONO_CEE_CALLVIRT || op == MONO_CEE_NEWOBJ;
}

/// Whether the translator answers a call to target out of the array it is
/// made on, such as Array.GetUpperBound ().
bool
is_array_shape_accessor (MonoMethod *target)
{
	// The class comes first because the signature does not: parsing one
	// resolves the types it names, and a body the pre-pass walks can name a
	// method whose parameter type is missing on purpose.
	if (target->klass != mono_defaults.array_class)
		return false;

	MonoMethodSignature *sig = mono_method_signature_internal (target);

	return sig != nullptr && is_array_shape_builtin (target, sig)
	       && is_external_method (target);
}

bool
is_fallthrough_branch (const unsigned char *code, MonoOpcodeEnum op, size_t operand)
{
	// A displacement is counted from the instruction behind the branch, so
	// zero targets that same instruction.
	if (op == MONO_CEE_BR_S)
		return (int8_t) code[operand] == 0;
	if (op == MONO_CEE_BR)
		return (int32_t) il_read_u32 (code + operand) == 0;

	return false;
}

/// Returns the method's shape when its IL is one straight line: at most one
/// call, not counting a call is_array_shape_accessor () names, nothing
/// blocks_a_fold () refuses, and a terminator on the last IL byte. Returns
/// nullopt otherwise.
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
std::optional<Shape>
match_trivial_shape (MonoMethod *method, MonoMethodHeader *header)
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

		if (is_call_opcode (op)) {
			MonoMethod *target =
				il_call_target (method, il_read_u32 (code + operand));

			if (target == nullptr)
				return std::nullopt;

			// An array-shape call has no frame for the one-call rule to
			// hold, so it does not count against it. Array.GetUpperBound ()
			// is the body this lets through: two of these and arithmetic.
			if (is_array_shape_accessor (target)) {
				at = next;
				continue;
			}

			if (shape.call != nullptr)
				return std::nullopt;

			shape.call = target;
		} else if (blocks_a_fold (op)
		           // A C# compiler ends a value-returning method with
		           // stloc.0, a branch to the next instruction, ldloc.0,
		           // then ret. Letting that one branch through as a
		           // fallthrough keeps such a getter or forwarder on one
		           // line.
		           && !is_fallthrough_branch (code, op, operand)) {
			return std::nullopt;
		}

		at = next;
	}

	return std::nullopt;
}

/// How far forwards_into_a_cycle () follows a chain of forwarders. Longer
/// than any chain worth following, and each link costs a header.
constexpr int max_links = 8;

/// Decides which of a caller's callees are worth folding in without weighing
/// them, and how much more of that translation this compile still has budget
/// for. materialize_trivial_callees () builds what this approves and moves
/// the caller's sites onto it.
class TrivialInlineAdvisor {
public:
	TrivialInlineAdvisor (MonoDomain *domain, InlineScope &scope, uint32_t il_limit)
	    : domain_ (domain), scope_ (scope), il_limit_ (il_limit),
	      fanout_limit_ (trivial_inline_fanout_limit ()),
	      instance_budget_ (trivial_inline_instance_budget ()),
	      instances_left_ (instance_budget_)
	{
	}

	/// Whether callee is worth the cost of fetching its header and weighing
	/// its shape. sites is how many of the caller's call sites name it, and
	/// rebuild says a copy for it already stands elsewhere in this compile.
	bool worth_a_copy (MonoMethod *callee, unsigned sites, bool rebuild) const;

	/// Whether callee's header is one of the shapes this pre-pass folds in
	/// without weighing it.
	bool fits_the_shape (MonoMethod *callee, MonoMethodHeader *header) const;

	/// Records that a copy built for sites call sites was kept.
	void charge (unsigned sites)
	{
		if (instance_budget_ != 0)
			instances_left_ -= sites;
	}

private:
	/// Whether target reaches itself through the forwarder chain
	/// match_trivial_shape () describes. A recursive body is one no inliner
	/// can fold the call out of, however small it is, so direct recursion
	/// and a cycle through other forwarders both refuse here.
	bool forwards_into_a_cycle (MonoMethod *target) const;

	MonoDomain *domain_;
	InlineScope &scope_;
	uint32_t il_limit_;
	uint32_t fanout_limit_;
	uint32_t instance_budget_;
	uint32_t instances_left_;
};

bool
TrivialInlineAdvisor::worth_a_copy (MonoMethod *callee, unsigned sites, bool rebuild) const
{
	// materialize_inline_copy () charges scope_.budget.trivial only the
	// first time it takes a method in, so a rebuild spends none of it.
	if (!rebuild && scope_.budget.trivial == 0)
		return false;

	// Reaching here always means a fresh copy is about to be built and its
	// sites duplicated, rebuild or not, so both limits below gate every one
	// of these rather than only a first-time fold.
	if (fanout_limit_ != 0 && sites > fanout_limit_)
		return false;
	if (instance_budget_ != 0 && sites > instances_left_)
		return false;

	return is_inlinable (domain_, callee);
}

bool
TrivialInlineAdvisor::fits_the_shape (MonoMethod *callee, MonoMethodHeader *header) const
{
	// A body the backend writes itself is never translated from its IL, so
	// none of the checks below apply to it.
	if (is_builtin (callee))
		return true;

	return is_small_and_clause_free (header, il_limit_)
	       && match_trivial_shape (callee, header) && !forwards_into_a_cycle (callee);
}

bool
TrivialInlineAdvisor::forwards_into_a_cycle (MonoMethod *target) const
{
	SmallPtrSet<MonoMethod *, max_links> seen;

	for (int link = 0; link < max_links && target != nullptr; ++link) {
		if (!seen.insert (target).second)
			return true;

		if (is_external_method (target))
			return false;

		ERROR_DECL (metadata_error);
		MinimalCompile cfg (target, domain_, metadata_error);
		MonoMethodHeader *header = cfg.get ()->header;

		if (header == nullptr) {
			mono_error_cleanup (metadata_error);
			return false;
		}

		// A body the shape test declines ends the chain here: it keeps its
		// own call sites, so a cycle behind it is one the pre-pass never
		// folds through.
		if (header->num_clauses != 0)
			return false;

		std::optional<Shape> shape = match_trivial_shape (target, header);

		if (!shape)
			return false;

		target = shape->call;
	}

	// A cycle longer than this walk is still caught at the site move:
	// copy_reaches () keeps a site on its call when the copy it would move
	// to already reaches the caller, so giving up here costs nothing.
	return false;
}

/// A copy belongs to the one compile that asked for it. Another body in the
/// module keeps the declaration and reaches the published entry, until its
/// own compile folds a copy of its own.
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
	char *caller_name = mono_method_full_name (caller, TRUE);
	char *callee_name = mono_method_full_name (callee, TRUE);

	MONO_LOCK (jit_trace_mutex ())
	{
		fprintf (stderr, "[llvm-jit] folding %s into %s\n", callee_name, caller_name);
	}
	g_free (callee_name);
	g_free (caller_name);
}

} // namespace

void
materialize_trivial_callees (Module &module, MonoDomain *domain, MonoMethod *root,
                             Function &body, std::vector<ExternalSymbol> &externals,
                             ModuleTypes &types, InlineScope &scope,
                             ResolveExternals resolve)
{
	uint32_t il_limit = trivial_inline_il_limit ();

	if (il_limit == 0 || folding_off_for_seq_points ())
		return;

	struct Candidate {
		MonoMethod *method;
		llvm::Function *body;

		/// Folds between this candidate and root. Zero is root itself.
		unsigned depth;
	};

	SmallVector<Candidate, 8> pending { { root, &body, 0 } };

	// Methods a fresh copy already failed to resolve, so a later site for
	// the same callee does not retry it.
	SmallPtrSet<MonoMethod *, 4> unresolved;

	TrivialInlineAdvisor advisor (domain, scope, il_limit);

	// Least deep first: the worklist is drained from the front, so a budget
	// that runs out drops the deepest candidates rather than whichever chain
	// the walk happened to go down.
	for (size_t next = 0; next < pending.size (); ++next) {
		auto [caller_method, caller_body, depth] = pending[next];

		// called keeps the order caller_body's instructions name each
		// declaration in, so a budget that runs out drops the same
		// candidates on every compile of this body.
		SmallVector<Function *, 8> called;
		SmallDenseMap<Function *, unsigned, 8> sites_of;

		for (Instruction &i : instructions (*caller_body)) {
			auto *site = dyn_cast<CallBase> (&i);
			Function *decl =
				site != nullptr ? site->getCalledFunction () : nullptr;

			if (decl == nullptr || !decl->isDeclaration ())
				continue;
			if (sites_of[decl]++ == 0)
				called.push_back (decl);
		}

		for (Function *decl : called) {
			// Each of decl's sites gets its own copy once AlwaysInlinerPass
			// folds it in, so the budgets below scale with this.
			unsigned sites = sites_of[decl];
			MonoMethod *callee = get_method (*decl);

			if (callee == nullptr || unresolved.contains (callee))
				continue;

			bool rebuild = already_folded (scope, callee);

			if (rebuild) {
				// Folding root back into itself would recurse forever.
				if (callee == scope.root)
					continue;

				Function *standing = folded_copy_in (scope, callee, module);

				// Ahead of the advisor: redirecting to a standing copy
				// costs no translation, so it happens even once the
				// budget is spent.
				if (standing != nullptr) {
					// A copy that already reaches caller_body keeps its
					// call: the two would fold into each other otherwise.
					if (!copy_reaches (*standing, *caller_body)) {
						g_assert (standing->getFunctionType ()
						          == decl->getFunctionType ());
						redirect_calls (*caller_body, *decl, *standing);
					}

					continue;
				}

				// The pipeline can erase a standing copy once every call
				// to it is folded, so reaching here for a rebuild is
				// ordinary rather than a bug to guard against.
			}

			if (!advisor.worth_a_copy (callee, sites, rebuild))
				continue;

			ERROR_DECL (metadata_error);
			MinimalCompile cfg (callee, domain, metadata_error);
			MonoMethodHeader *header = cfg.get ()->header;

			if (header == nullptr) {
				mono_error_cleanup (metadata_error);
				continue;
			}

			if (!advisor.fits_the_shape (callee, header))
				continue;

			size_t before = externals.size ();
			Function *copy =
				materialize_inline_copy (module, domain, callee, cfg.get (),
			                                 externals, types, scope,
			                                 Inliner::trivial);

			if (copy == nullptr)
				continue;

			// A class the copy names may fail to load; that failure
			// belongs at the call, inside whatever try wraps it, not at
			// root's entry. Erasing the copy leaves the call on the
			// callee's thunk, so the callee's own compile raises it there
			// instead.
			if (Error err = resolve (ArrayRef (externals).drop_front (before))) {
				consumeError (std::move (err));
				externals.resize (before);
				copy->eraseFromParent ();
				unresolved.insert (callee);
				continue;
			}

			/*
			 * A shared body is entered with its context in a register
			 * and a call to it is not, which is the one shape the copy
			 * and the declaration disagree on. Redirecting the site
			 * would run the copy on the wrong arguments, so leave the
			 * call on the callee's thunk.
			 */
			if (copy->getFunctionType () != decl->getFunctionType ()) {
				externals.resize (before);
				copy->eraseFromParent ();
				unresolved.insert (callee);
				continue;
			}

			if (is_jit_trace_enabled ())
				trace_inline (callee, caller_method);

			redirect_calls (*caller_body, *decl, *copy);
			advisor.charge (sites);

			// These shapes have nothing to weigh, so the pipeline folds
			// them rather than a cost model.
			copy->addFnAttr (Attribute::AlwaysInline);

			// A rebuild already had its own callees weighed the first time
			// this root walked it, so walking it again would spend budget
			// deeper rather than on the sites in hand.
			if (!rebuild && depth + 1 < trivial_inline_depth_limit ())
				pending.push_back ({ callee, copy, depth + 1 });
		}
	}
}

} // namespace mono
