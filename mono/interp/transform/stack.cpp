/**
 * \file
 * \brief Where a value lives while the transform is running.
 *
 * The evaluation stack, the locals it spills into, the method's own arguments
 * and locals, and the data-item table an instruction names a pointer through.
 */

#include "config.h"

#include <mono/metadata/class-internals.h>
#include <mono/metadata/exception-internals.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/tabledefs.h>

#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "runtime/internals.hpp"
#include "transform.hpp"
#include "internal.hpp"

namespace mono::interp {

void
TransformData::realloc_stack ()
{
	int sppos = sp - stack;

	stack_capacity *= 2;
	stack = (StackInfo *) g_realloc (stack, stack_capacity * sizeof (stack[0]));
	sp = stack + sppos;
}

int
TransformData::get_tos_offset ()
{
	if (sp == stack)
		return 0;
	else
		return sp[-1].offset + sp[-1].size;
}

static MonoType *
get_type_from_stack (StackType type, MonoClass *klass)
{
	switch (type) {
	case StackType::I4:
		return m_class_get_byval_arg (mono_defaults.int32_class);
	case StackType::I8:
		return m_class_get_byval_arg (mono_defaults.int64_class);
	case StackType::R4:
		return m_class_get_byval_arg (mono_defaults.single_class);
	case StackType::R8:
		return m_class_get_byval_arg (mono_defaults.double_class);
	case StackType::O:
		return (klass && !m_class_is_valuetype (klass))
		           ? m_class_get_byval_arg (klass)
		           : m_class_get_byval_arg (mono_defaults.object_class);
	case StackType::VT:
		return m_class_get_byval_arg (klass);
	case StackType::MP:
	case StackType::F:
		return m_class_get_byval_arg (mono_defaults.int_class);
	default:
		g_assert_not_reached ();
	}
}

/// Creates an additional local, allocated past the method's own locals, so it
/// is addressed the same way: at an offset from the frame's locals area.
int
TransformData::create_interp_local_explicit (MonoType *type, int size)
{
	InterpLocal local{};

	local.type = type;
	local.mt = mint_type (type);
	local.offset = -1;
	local.size = size;

	locals.push_back (local);
	return (int) locals.size () - 1;
}

int
TransformData::create_interp_stack_local (StackType type, MonoClass *k, int type_size, int offset)
{
	int local = create_interp_local_explicit (get_type_from_stack (type, k), type_size);

	locals[local].flags |= INTERP_LOCAL_FLAG_EXECUTION_STACK;
	locals[local].stack_offset = offset;
	return local;
}

void
TransformData::push_type_explicit (StackType type, MonoClass *k, int type_size)
{
	int sp_height;
	sp_height = sp - stack + 1;
	if (sp_height > max_stack_height)
		max_stack_height = sp_height;
	if (sp_height > stack_capacity)
		realloc_stack ();
	sp->type = type;
	sp->klass = k;
	sp->flags = 0;
	sp->offset = get_tos_offset ();
	sp->local = create_interp_stack_local (type, k, type_size, sp->offset);
	sp->size = ALIGN_TO (type_size, MINT_STACK_SLOT_SIZE);
	if ((sp->size + sp->offset) > max_stack_size)
		max_stack_size = sp->size + sp->offset;
	sp++;
}

// This does not handle the size/offset of the entry. For those cases
// we need to manually pop the top of the stack and push a new entry.
void
TransformData::set_type_and_local (StackInfo *sp, MonoClass *klass, StackType type)
{
	SET_TYPE (sp, type, klass);
	sp->local = create_interp_stack_local (type, NULL, MINT_STACK_SLOT_SIZE, sp->offset);
}

void
TransformData::set_simple_type_and_local (StackInfo *sp, StackType type)
{
	set_type_and_local (sp, NULL, type);
}

void
TransformData::push_type (StackType type, MonoClass *k)
{
	// We don't really care about the exact size for non-valuetypes
	push_type_explicit (type, k, MINT_STACK_SLOT_SIZE);
}

void
TransformData::push_simple_type (StackType type)
{
	push_type (type, NULL);
}

void
TransformData::push_type_vt (MonoClass *k, int size)
{
	push_type_explicit (StackType::VT, k, size);
}

void
TransformData::push_types (StackInfo *types, int count)
{
	for (int i = 0; i < count; i++)
		push_type_explicit (types[i].type, types[i].klass, types[i].size);
}

static int
can_store (StackType st_value, StackType vt_value)
{
	if (st_value == StackType::O || st_value == StackType::MP)
		st_value = StackType::I;
	if (vt_value == StackType::O || vt_value == StackType::MP)
		vt_value = StackType::I;
	return st_value == vt_value;
}

MonoType *
TransformData::get_arg_type_exact (int n, MintType *mt)
{
	MonoType *type;
	gboolean hasthis = mono_method_signature_internal (method)->hasthis;

	if (hasthis && n == 0)
		type = m_class_get_byval_arg (method->klass);
	else
		type = mono_method_signature_internal (method)->params[n - !!hasthis];

	if (mt)
		*mt = mint_type (type);

	return type;
}

/// Returns the magic class of type, or null if it is not one.
///
/// A stack entry carrying one holds the value rather than a pointer to it.
/// CEE_LDFLD reads such an entry instead of dereferencing it.
MonoClass *
magic_class_of (MonoType *type)
{
	MonoClass *klass;

	if (type->byref || type->type != MONO_TYPE_VALUETYPE)
		return NULL;

	klass = mono_class_from_mono_type_internal (type);

	return mono_class_get_magic_index (klass) >= 0 ? klass : NULL;
}

void
TransformData::load_arg (int n)
{
	gint32 size = 0;
	MintType mt;
	MonoClass *klass = NULL;
	MonoType *type;
	gboolean hasthis = mono_method_signature_internal (method)->hasthis;

	type = get_arg_type_exact (n, &mt);

	if (mt == MintType::VT) {
		klass = mono_class_from_mono_type_internal (type);
		if (mono_method_signature_internal (method)->pinvoke)
			size = mono_class_native_size (klass, NULL);
		else
			size = mono_class_value_size (klass, NULL);

		if (hasthis && n == 0) {
			mt = MintType::I;
			klass = NULL;
			push_type (stack_type_of (mt), klass);
		} else {
			g_assert (size < G_MAXUINT16);
			push_type_vt (klass, size);
		}
	} else {
		/* Only an instance method's receiver is a pointer to the value, and the
		 * pointer-sized case below covers a static method's first argument as
		 * well. */
		MonoClass *magic = hasthis && n == 0 ? NULL : magic_class_of (type);

		if ((hasthis || mt == MintType::I) && n == 0) {
			// Special case loading of the first ptr sized argument
			if (mt != MintType::O)
				mt = MintType::I;
			klass = magic;
		} else {
			if (mt == MintType::O)
				klass = mono_class_from_mono_type_internal (type);
			else
				klass = magic;
		}
		push_type (stack_type_of (mt), klass);
	}
	interp_add_ins (get_mov_for_type (mt, TRUE));
	interp_ins_set_sreg (last_ins, n);
	interp_ins_set_dreg (last_ins, sp[-1].local);
	if (mt == MintType::VT)
		last_ins->data[0] = size;
}

void
TransformData::store_arg (int n)
{
	gint32 size = 0;
	MintType mt;
	CHECK_STACK (1);
	MonoType *type;

	type = get_arg_type_exact (n, &mt);

	if (mt == MintType::VT) {
		MonoClass *klass = mono_class_from_mono_type_internal (type);
		if (mono_method_signature_internal (method)->pinvoke)
			size = mono_class_native_size (klass, NULL);
		else
			size = mono_class_value_size (klass, NULL);
		g_assert (size < G_MAXUINT16);
	}
	coerce_fp (sp - 1, stack_type_of (mt));
	--sp;
	interp_add_ins (get_mov_for_type (mt, FALSE));
	interp_ins_set_sreg (last_ins, sp[0].local);
	interp_ins_set_dreg (last_ins, n);
	if (mt == MintType::VT)
		last_ins->data[0] = size;
}

void
TransformData::load_local (int local)
{
	MintType mt = locals[local].mt;
	gint32 size = locals[local].size;
	MonoType *type = locals[local].type;

	if (mt == MintType::VT) {
		MonoClass *klass = mono_class_from_mono_type_internal (type);
		push_type_vt (klass, size);
	} else {
		MonoClass *klass = NULL;
		if (mt == MintType::O)
			klass = mono_class_from_mono_type_internal (type);
		push_type (stack_type_of (mt), klass);
	}
	interp_add_ins (get_mov_for_type (mt, TRUE));
	interp_ins_set_sreg (last_ins, local);
	interp_ins_set_dreg (last_ins, sp[-1].local);
	if (mt == MintType::VT)
		last_ins->data[0] = size;
}

void
TransformData::store_local (int local)
{
	MintType mt = locals[local].mt;
	CHECK_STACK (1);
#if SIZEOF_VOID_P == 8
	if (sp[-1].type == StackType::I4 && stack_type_of (mt) == StackType::I8)
		interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
#endif
	coerce_fp (sp - 1, stack_type_of (mt));
	if (!can_store (sp[-1].type, stack_type_of (mt))) {
		g_warning ("%s.%s: Store local stack type mismatch %d %d", m_class_get_name (method->klass),
		           method->name, (int) stack_type_of (mt), (int) sp[-1].type);
	}
	--sp;
	interp_add_ins (get_mov_for_type (mt, FALSE));
	interp_ins_set_sreg (last_ins, sp[0].local);
	interp_ins_set_dreg (last_ins, local);
	if (mt == MintType::VT)
		last_ins->data[0] = locals[local].size;
}

guint16
TransformData::get_data_item_index_nonshared (void *ptr)
{
	data_items.push_back (ptr);
	return (guint16) (data_items.size () - 1);
}

guint16
TransformData::get_data_item_index (void *ptr)
{
	auto known = data_hash.find (ptr);

	if (known != data_hash.end ())
		return known->second;

	guint16 index = get_data_item_index_nonshared (ptr);

	data_hash[ptr] = index;
	return index;
}

int
mono_class_get_magic_index (MonoClass *k)
{
	if (mono_class_is_magic_int (k))
		return !strcmp ("nint", m_class_get_name (k)) ? 0 : 1;

	if (mono_class_is_magic_float (k))
		return 2;

	return -1;
}

int
TransformData::create_interp_local (MonoType *type)
{
	int size, align;

	size = mono_type_size (type, &align);
	g_assert (align <= MINT_STACK_SLOT_SIZE);

	return create_interp_local_explicit (type, size);
}

int
TransformData::get_interp_local_offset (int local, gboolean resolve_stack_locals)
{
	// FIXME MINT_PROF_EXIT when void
	if (local == -1)
		return -1;

	if ((locals[local].flags & INTERP_LOCAL_FLAG_EXECUTION_STACK) && !resolve_stack_locals)
		return -1;

	if (locals[local].offset != -1)
		return locals[local].offset;

	if (locals[local].flags & INTERP_LOCAL_FLAG_EXECUTION_STACK) {
		locals[local].offset = total_locals_size + locals[local].stack_offset;
	} else {
		int size, offset;

		offset = total_locals_size;
		size = locals[local].size;

		locals[local].offset = offset;

		total_locals_size = ALIGN_TO (offset + size, MINT_STACK_SLOT_SIZE);
	}

	//g_assert (total_locals_size < G_MAXUINT16);

	return locals[local].offset;
}

void
TransformData::interp_method_compute_offsets (InterpMethod *imethod, MonoMethodSignature *sig,
                                              MonoMethodHeader *header, MonoError *error)
{
	int i, offset, size, align;
	int num_args = sig->hasthis + sig->param_count;
	int num_il_locals = header->num_locals;
	int num_locals = num_args + num_il_locals;

	imethod->local_offsets = (guint32 *) g_malloc (num_il_locals * sizeof (guint32));
	locals.resize (num_locals);
	offset = 0;

	g_assert (MINT_STACK_SLOT_SIZE == MINT_VT_ALIGNMENT);

	/*
	 * We will load arguments as if they are locals. Unlike normal locals, every argument
	 * is stored in a stackval sized slot and valuetypes have special semantics since we
	 * receive a pointer to the valuetype data rather than the data itself.
	 */
	for (i = 0; i < num_args; i++) {
		MonoType *type;
		if (sig->hasthis && i == 0)
			type = m_class_get_byval_arg (method->klass);
		else
			type = mono_method_signature_internal (method)->params[i - sig->hasthis];
		MintType mt = mint_type (type);
		locals[i].type = type;
		locals[i].offset = offset;
		locals[i].flags = 0;
		locals[i].indirects = 0;
		locals[i].mt = mt;
		if (mt == MintType::VT && (!sig->hasthis || i != 0)) {
			size = mono_type_size (type, &align);
			locals[i].size = size;
			offset += ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		} else {
			locals[i].size = MINT_STACK_SLOT_SIZE; // not really
			offset += MINT_STACK_SLOT_SIZE;
		}
	}

	il_locals_offset = offset;
	for (i = 0; i < num_il_locals; ++i) {
		int index = num_args + i;
		size = mono_type_size (header->locals[i], &align);
		if (header->locals[i]->type == MONO_TYPE_VALUETYPE) {
			if (mono_class_has_failure (header->locals[i]->data.klass)) {
				mono_error_set_for_class_failure (error, header->locals[i]->data.klass);
				return;
			}
		}
		offset += align - 1;
		offset &= ~(align - 1);
		imethod->local_offsets[i] = offset;
		locals[index].type = header->locals[i];
		locals[index].offset = offset;
		locals[index].flags = 0;
		locals[index].indirects = 0;
		locals[index].mt = mint_type (header->locals[i]);
		if (locals[index].mt == MintType::VT)
			locals[index].size = size;
		else
			locals[index].size = MINT_STACK_SLOT_SIZE; // not really
		// Every local takes a MINT_STACK_SLOT_SIZE so IL locals have same behavior as execution locals
		offset += ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
	}
	offset = ALIGN_TO (offset, MINT_VT_ALIGNMENT);
	il_locals_size = offset - il_locals_offset;

	imethod->clause_data_offsets = (guint32 *) g_malloc (header->num_clauses * sizeof (guint32));
	for (i = 0; i < header->num_clauses; i++) {
		imethod->clause_data_offsets[i] = offset;
		offset += sizeof (MonoObject *);
	}
	offset = ALIGN_TO (offset, MINT_VT_ALIGNMENT);

	//g_assert (offset < G_MAXUINT16);
	total_locals_size = offset;
}

} // namespace mono::interp
