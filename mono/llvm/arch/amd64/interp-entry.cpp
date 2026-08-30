/**
 * \file
 * \brief Reading a call out of the registers it arrived in, for the interpreter.
 *
 * The convention restated here is LLVM's own lowering of the backend's ccc
 * declarations - CC_X86_64_C and RetCC_X86_64_C in X86CallingConv.td - and not
 * the SysV classification that mono-abi.cpp restates. The two differ
 * where it matters most: LLVM flattens an aggregate argument into its scalar
 * leaves and assigns each leaf a register of its own, so
 *
 *     struct { int a; float b; }     travels in one integer and one SSE register
 *     struct { byte a, b, c, d; }    travels in four integer registers
 *
 * where the C ABI packs each into a single eightbyte. Padding a managed
 * layout spells out is a struct member like any other to that lowering, and
 * takes a register too.
 *
 * The register files run down independently, and a leaf that finds its file
 * empty goes to the stack while later leaves of other files still get
 * registers. So an aggregate can straddle the boundary, and one that is wholly
 * on the stack is still one slot per leaf rather than an image of the value.
 */

/*
 * Before anything else, so that MonoError is the internal struct the rest of
 * the runtime passes around rather than the opaque public one.
 */
#include "runtime-error.hpp"

#include "arch/amd64/leaf-layout.hpp"
#include "arch/arch.hpp"
#include "hidden-return.hpp"
#include "interp-entry.hpp"

#include "domain-method.hpp"
#include "mini.h"
#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MathExtras.h>

#include <cstring>
#include <mutex>

using namespace llvm;

extern "C" {
void mono_llvm_interp_entry_from_context (mono::MonoDomainMethod *published,
                                          mono::arch::InterpArgContext *ctx);

extern char mono_llvm_interp_entry_thunk[];
extern char mono_llvm_interp_entry_thunk_pushed[];
extern char mono_llvm_interp_entry_thunk_framed[];
extern char mono_llvm_interp_entry_thunk_popped[];
extern char mono_llvm_interp_entry_thunk_end[];
}

namespace mono::arch {

namespace {

Error
unsupported (const Twine &what)
{
	return createStringError (inconvertibleErrorCode (),
	                          "the interpreter entry cannot carry " + what);
}

/// Assign one parameter's leaves and say how to read the value back.
Expected<ArgPlan>
place_parameter (Type *param, bool byref, LeafAssigner &assign, const DataLayout &dl,
                 std::vector<ArgPiece> &pieces)
{
	SmallVector<Leaf, 8> leaves;

	if (Error err = flatten (param, 0, dl, leaves))
		return std::move (err);

	ArgPlan plan;

	plan.byref = byref;
	plan.size = (uint32_t) dl.getTypeStoreSize (param).getFixedValue ();

	SmallVector<ArgPiece, 8> placed;

	for (const Leaf &leaf : leaves)
		placed.push_back (assign.place (leaf, dl));

	if (placed.size () == 1 && placed[0].offset == 0 && placed[0].width == plan.size) {
		switch (placed[0].file) {
		case ArgPiece::File::Greg:
			plan.where = ArgPlan::Where::Greg;
			break;
		case ArgPiece::File::Freg:
			plan.where = ArgPlan::Where::Freg;
			break;
		case ArgPiece::File::Stack:
			plan.where = ArgPlan::Where::Stack;
			break;
		}
		plan.at = placed[0].at;
		return plan;
	}

	plan.where = ArgPlan::Where::Pieces;
	plan.first_piece = (uint32_t) pieces.size ();
	plan.piece_count = (uint32_t) placed.size ();
	pieces.insert (pieces.end (), placed.begin (), placed.end ());
	return plan;
}

/// What every gathered value is aligned to. A value type with a vector field
/// wants this much.
constexpr uint64_t scratch_alignment = 16;

/// Give every value that has to be gathered somewhere to be gathered, and say
/// how much room that takes altogether.
uint32_t
lay_out_scratch (InterpEntryLayout &layout)
{
	uint32_t at = 0;
	auto reserve = [&at] (uint32_t size) {
		uint32_t here = at;

		at = (uint32_t) alignTo (at + std::max (size, 1u), scratch_alignment);
		return here;
	};

	for (ArgPlan &plan : layout.args)
		if (plan.where == ArgPlan::Where::Pieces)
			plan.at = reserve (plan.size);

	if (layout.ret.kind == ReturnPlan::Kind::Registers)
		layout.ret_scratch = reserve (layout.ret.size);

	return at;
}

uint8_t *
piece_address (const ArgPiece &piece, InterpArgContext *ctx)
{
	switch (piece.file) {
	case ArgPiece::File::Greg:
		return (uint8_t *) &ctx->gregs[piece.at];
	case ArgPiece::File::Freg:
		return ctx->fregs[piece.at];
	case ArgPiece::File::Stack:
		return ctx->stack + piece.at;
	}

	llvm_unreachable ("an argument piece with no register file");
}

} // namespace

Expected<InterpEntryLayout>
plan_interp_entry (Function *shape, MonoMethodSignature *sig)
{
	/*
	 * The variable part of a vararg call travels in a cookie buffer that the
	 * interpreter's own entry knows nothing about.
	 */
	if (sig->call_convention == MONO_CALL_VARARG)
		return unsupported ("a vararg signature");

	const DataLayout &dl = shape->getParent ()->getDataLayout ();
	FunctionType *type = shape->getFunctionType ();
	Type *hidden = hidden_return_type (shape);
	unsigned params = type->getNumParams ();
	unsigned hidden_at = hidden != nullptr ? hidden_return_index (placed_parameter_count (shape))
	                                       : params;
	unsigned natural = hidden != nullptr ? params - 1 : params;

	if (natural != (unsigned) (sig->hasthis + sig->param_count))
		return unsupported ("a prototype the signature does not account for");

	InterpEntryLayout layout;
	LeafAssigner assign;
	std::vector<ArgPlan> plans (natural);
	unsigned hidden_greg = 0;

	for (unsigned p = 0; p < params; ++p) {
		if (p == hidden_at) {
			Leaf leaf { 0, type->getParamType (p) };
			ArgPiece piece = assign.place (leaf, dl);

			/*
			 * Only ever parameter 0 or 1, so the integer file cannot have run
			 * out underneath it, and the runtime's trampolines read it out of a
			 * register.
			 */
			if (piece.file != ArgPiece::File::Greg)
				return unsupported ("a hidden return pointer that missed a "
				                    "register");
			hidden_greg = piece.at;
			continue;
		}

		unsigned i = p < hidden_at ? p : p - 1;
		bool receiver = sig->hasthis && i == 0;
		bool byref = !receiver && sig->params[i - sig->hasthis]->byref;
		Expected<ArgPlan> plan = place_parameter (type->getParamType (p), byref,
		                                          assign, dl, layout.pieces);

		if (!plan)
			return plan.takeError ();
		plans[i] = *plan;
	}

	layout.has_this = sig->hasthis;

	if (sig->hasthis) {
		// The receiver leads the prototype, so it reaches a register or nothing does.
		if (plans[0].where != ArgPlan::Where::Greg)
			return unsupported ("a receiver that missed a register");
		layout.this_greg = plans[0].at;
	}

	layout.args.assign (plans.begin () + sig->hasthis, plans.end ());

	if (hidden != nullptr) {
		layout.ret.kind = ReturnPlan::Kind::Hidden;
		layout.ret.hidden_greg = hidden_greg;
	} else {
		Expected<ReturnPlan> ret = place_return (type->getReturnType (), dl);

		if (!ret)
			return ret.takeError ();
		layout.ret = std::move (*ret);
	}

	layout.scratch_size = lay_out_scratch (layout);
	return layout;
}

void *
interp_entry_thunk ()
{
	static std::once_flag once;

	std::call_once (once, [] {
		char *start = mono_llvm_interp_entry_thunk;
		GSList *ops = nullptr;

		mono_add_unwind_op_def_cfa (ops, start, start, AMD64_RSP, 8);
		mono_add_unwind_op_offset (ops, start, start, AMD64_RIP, -8);
		mono_add_unwind_op_def_cfa_offset (ops, mono_llvm_interp_entry_thunk_pushed,
		                                  start, 16);
		mono_add_unwind_op_offset (ops, mono_llvm_interp_entry_thunk_pushed, start,
		                           AMD64_RBP, -16);
		mono_add_unwind_op_def_cfa_reg (ops, mono_llvm_interp_entry_thunk_framed,
		                                start, AMD64_RBP);
		mono_add_unwind_op_same_value (ops, mono_llvm_interp_entry_thunk_popped,
		                               start, AMD64_RBP);
		mono_add_unwind_op_def_cfa (ops, mono_llvm_interp_entry_thunk_popped, start,
		                            AMD64_RSP, 8);

		guint32 length = 0;
		guint8 *encoded = mono_unwind_ops_encode (ops, &length);
		MonoTrampInfo *info = g_new0 (MonoTrampInfo, 1);

		info->code = (guint8 *) start;
		info->code_size = (guint32) (mono_llvm_interp_entry_thunk_end - start);
		info->name = g_strdup ("llvm_interp_entry");
		info->uw_info = encoded;
		info->uw_info_len = length;
		mono_tramp_info_register (info, nullptr);
		mono_free_unwind_info (ops);
	});

	return mono_llvm_interp_entry_thunk;
}

} // namespace mono::arch

using namespace mono;
using namespace mono::arch;

/**
 * What interp-entry-thunk.S calls once it has spilled the call.
 */
extern "C" void
mono_llvm_interp_entry_from_context (mono::MonoDomainMethod *published, InterpArgContext *ctx)
{
	llvm::Expected<InterpEntryPoint> found = interp_entry_for (published);

	if (!found) {
		char *name = mono_method_full_name (published->method, TRUE);
		std::string why = llvm::toString (found.takeError ());

		g_error ("no interpreter entry for %s: %s", name, why.c_str ());
	}

	InterpEntryPoint entry = *found;
	const InterpEntryLayout &layout = *entry.layout;
	SmallVector<void *, 16> args (layout.args.size ());
	SmallVector<uint8_t, 256> scratch (layout.scratch_size);

	for (size_t i = 0; i < layout.args.size (); ++i) {
		const ArgPlan &plan = layout.args[i];
		uint8_t *value = nullptr;

		switch (plan.where) {
		case ArgPlan::Where::Greg:
			value = (uint8_t *) &ctx->gregs[plan.at];
			break;
		case ArgPlan::Where::Freg:
			value = ctx->fregs[plan.at];
			break;
		case ArgPlan::Where::Stack:
			value = ctx->stack + plan.at;
			break;
		case ArgPlan::Where::Pieces:
			value = scratch.data () + plan.at;
			for (uint32_t p = 0; p < plan.piece_count; ++p) {
				const ArgPiece &piece = layout.pieces[plan.first_piece + p];

				memcpy (value + piece.offset, piece_address (piece, ctx),
				        piece.width);
			}
			break;
		}

		args[i] = plan.byref ? *(void **) value : value;
	}

	void *this_arg = layout.has_this ? (void *) ctx->gregs[layout.this_greg] : nullptr;
	void *res = nullptr;

	switch (layout.ret.kind) {
	case ReturnPlan::Kind::None:
		break;
	case ReturnPlan::Kind::Hidden:
		res = (void *) ctx->gregs[layout.ret.hidden_greg];
		break;
	case ReturnPlan::Kind::Registers:
		res = scratch.data () + layout.ret_scratch;
		break;
	}

	alignas (16) uint8_t frame[interp_frame_size];

	interp_frame_enter (frame, ctx);
	mini_get_interp_callbacks ()->entry_from_args (entry.imethod, this_arg, res,
	                                              args.data ());
	interp_frame_leave (frame);

	switch (layout.ret.kind) {
	case ReturnPlan::Kind::None:
		break;
	case ReturnPlan::Kind::Hidden:
		// The convention hands a hidden return pointer back in rax.
		ctx->ret_gregs[0] = (uint64_t) res;
		break;
	case ReturnPlan::Kind::Registers:
		for (const ArgPiece &piece : layout.ret.pieces) {
			uint8_t *to = piece.file == ArgPiece::File::Freg
			                      ? ctx->ret_fregs[piece.at]
			                      : (uint8_t *) &ctx->ret_gregs[piece.at];

			memcpy (to, (uint8_t *) res + piece.offset, piece.width);
		}
		break;
	}
}
