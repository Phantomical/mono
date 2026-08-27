/**
 * \file
 * \brief Making a call whose prototype the caller only knows at run time.
 *
 * The convention restated here is the one interp-entry.cpp reads a call out
 * of, in the other direction. It is LLVM's own lowering of the backend's ccc
 * declarations, not the SysV classification that mini's own dyn-call code is
 * built on. The two agree for scalar arguments and part company over
 * aggregates, so plan_dyn_call () refuses an aggregate rather than pick a
 * side.
 *
 * That refusal is what keeps this file short. Every leaf is one register wide.
 * So the two register files run down in argument order, the overflow takes one
 * stack word each, and no return is indirect.
 */

/*
 * Before anything else, so that MonoError is the internal struct the rest of
 * the runtime passes around rather than the opaque public one.
 */
#include "runtime-error.hpp"

#include "arch/arch.hpp"

#include "mini.h"

#include "mono/metadata/class-internals.h"

#include <cstring>

extern "C" {
void mono_llvm_dyn_call_thunk (mono::arch::DynCallFrame *frame, void *target);
}

namespace mono::arch {

namespace {

/// What the frame has room for. A leaf past either file goes to the stack.
constexpr unsigned greg_file = 6;
constexpr unsigned freg_file = 8;

/// The register files and the stack, run down as the arguments are placed.
struct Assigner {
	unsigned greg = 0;
	unsigned freg = 0;
	unsigned stack = 0;
};

/// Names \p t as one scalar leaf, or returns false for a type that is not one.
///
/// A byref is a pointer whatever it points at, so it is answered before the
/// type itself is looked at.
bool
scalar_load (MonoType *t, DynCallArg::Load *load, bool *sse)
{
	*sse = false;

	if (t->byref) {
		*load = DynCallArg::Load::I8;
		return true;
	}

	/* Enums, bool, char and the native integer types all come back as the
	 * integer the call actually passes. */
	t = mini_get_underlying_type (t);

	switch (t->type) {
	case MONO_TYPE_I1:
		*load = DynCallArg::Load::I1;
		return true;
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_U1:
		*load = DynCallArg::Load::U1;
		return true;
	case MONO_TYPE_I2:
		*load = DynCallArg::Load::I2;
		return true;
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U2:
		*load = DynCallArg::Load::U2;
		return true;
	case MONO_TYPE_I4:
		*load = DynCallArg::Load::I4;
		return true;
	case MONO_TYPE_U4:
		*load = DynCallArg::Load::U4;
		return true;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:
		*load = DynCallArg::Load::I8;
		return true;
	case MONO_TYPE_R4:
		*load = DynCallArg::Load::R4;
		*sse = true;
		return true;
	case MONO_TYPE_R8:
		*load = DynCallArg::Load::R8;
		*sse = true;
		return true;
	case MONO_TYPE_GENERICINST:
		/* An instantiation of a class is a reference like any other. One of
		 * a value type is the aggregate this refuses. */
		if (MONO_TYPE_IS_REFERENCE (t)) {
			*load = DynCallArg::Load::I8;
			return true;
		}
		return false;
	default:
		return false;
	}
}

/// Gives one leaf the next slot of its file, or the next stack word.
DynCallArg
place_scalar (DynCallArg::Load load, bool sse, Assigner &assign)
{
	DynCallArg arg;

	arg.load = load;

	if (sse && assign.freg < freg_file) {
		arg.file = DynCallArg::File::Freg;
		arg.at = assign.freg++;
	} else if (!sse && assign.greg < greg_file) {
		arg.file = DynCallArg::File::Greg;
		arg.at = assign.greg++;
	} else {
		arg.file = DynCallArg::File::Stack;
		arg.at = assign.stack++;
	}

	return arg;
}

/// How many bytes the interpreter is owed through the return pointer.
uint8_t
return_width (DynCallArg::Load load)
{
	switch (load) {
	case DynCallArg::Load::I1:
	case DynCallArg::Load::U1:
		return 1;
	case DynCallArg::Load::I2:
	case DynCallArg::Load::U2:
		return 2;
	case DynCallArg::Load::I4:
	case DynCallArg::Load::U4:
	case DynCallArg::Load::R4:
		return 4;
	default:
		return 8;
	}
}

/// Reads one argument out of the interpreter's storage, widened to the whole
/// register slot the call passes it in.
uint64_t
read_argument (const DynCallArg &arg, const void *from)
{
	switch (arg.load) {
	case DynCallArg::Load::I1:
		return (uint64_t) (int64_t) *(const int8_t *) from;
	case DynCallArg::Load::U1:
		return *(const uint8_t *) from;
	case DynCallArg::Load::I2:
		return (uint64_t) (int64_t) *(const int16_t *) from;
	case DynCallArg::Load::U2:
		return *(const uint16_t *) from;
	case DynCallArg::Load::I4:
		return (uint64_t) (int64_t) *(const int32_t *) from;
	case DynCallArg::Load::U4:
		return *(const uint32_t *) from;
	case DynCallArg::Load::R4: {
		uint32_t bits;

		memcpy (&bits, from, sizeof (bits));
		return bits;
	}
	default: {
		uint64_t bits;

		memcpy (&bits, from, sizeof (bits));
		return bits;
	}
	}
}

} // namespace

std::unique_ptr<DynCallPlan>
plan_dyn_call (MonoMethodSignature *sig, llvm::StringRef *why)
{
	auto plan = std::make_unique<DynCallPlan> ();
	Assigner assign;

	/*
	 * A receiver is always the first argument, which is what
	 * mono_arch_get_this_arg_from_call () and the unbox trampoline rest on.
	 */
	if (sig->hasthis)
		plan->args.push_back (place_scalar (DynCallArg::Load::I8, false, assign));

	for (int i = 0; i < sig->param_count; ++i) {
		DynCallArg::Load load;
		bool sse;

		if (!scalar_load (sig->params[i], &load, &sse)) {
			*why = "an argument is a value type";
			return nullptr;
		}

		plan->args.push_back (place_scalar (load, sse, assign));
	}

	if (sig->ret->type != MONO_TYPE_VOID) {
		DynCallArg::Load load;
		bool sse;

		if (!scalar_load (sig->ret, &load, &sse)) {
			*why = "the return is a value type";
			return nullptr;
		}

		plan->ret.file = sse ? DynCallReturn::File::Freg : DynCallReturn::File::Greg;
		plan->ret.width = return_width (load);
	}

	plan->wants_fp = assign.freg > 0;
	plan->stack_words = assign.stack;
	plan->frame_size =
		(uint32_t) (sizeof (DynCallFrame) + (assign.stack * sizeof (uint64_t)));

	return plan;
}

void
dyn_call (const DynCallPlan &plan, void *target, void **args, void *ret, void *frame)
{
	auto *f = (DynCallFrame *) frame;

	/*
	 * Only the slots the plan names are written. A register the call does not
	 * pass an argument in rides as whatever the frame held, which no callee
	 * reads, and zeroing the rest would cost a memset on every call.
	 */
	f->has_fp = plan.wants_fp;
	f->nstack = plan.stack_words;

	for (size_t i = 0; i < plan.args.size (); ++i) {
		const DynCallArg &arg = plan.args[i];
		uint64_t bits = read_argument (arg, args[i]);

		switch (arg.file) {
		case DynCallArg::File::Greg:
			f->gregs[arg.at] = bits;
			break;
		case DynCallArg::File::Freg:
			memcpy (&f->fregs[arg.at], &bits, sizeof (bits));
			break;
		case DynCallArg::File::Stack:
			f->stack[arg.at] = bits;
			break;
		}
	}

	mono_llvm_dyn_call_thunk (f, target);

	switch (plan.ret.file) {
	case DynCallReturn::File::None:
		break;
	case DynCallReturn::File::Greg:
		memcpy (ret, &f->ret_greg, plan.ret.width);
		break;
	case DynCallReturn::File::Freg:
		memcpy (ret, &f->ret_freg, plan.ret.width);
		break;
	}
}

} // namespace mono::arch
