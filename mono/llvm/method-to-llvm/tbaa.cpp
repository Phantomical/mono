/**
 * \file
 * \brief The `!tbaa` tree the translator tags managed accesses with.
 *
 * ManagedAccess (method-to-llvm.hpp) says what an access knows about its slot.
 * This file turns that into metadata, and builds the type descriptors the fine
 * tags are written against.
 */

#include "method-to-llvm.hpp"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/metadata-internals.h"
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>

#include <algorithm>
#include <string>
#include <vector>

namespace mono {

namespace {

/// One member of a type descriptor: its type, where it starts, how many bytes it
/// covers, and whether the descriptor names it.
///
/// A reference field and a nested value type take space without being named. A
/// reference is on its own leaf, and a field inside a nested value type is
/// tagged against that type's own descriptor, so no tag reaches either offset.
/// Their bytes are still counted, or a scalar overlapping one reads as
/// disjoint.
struct Member {
	int32_t offset;
	int32_t size;
	bool named;
	MonoType *type;
};

} // namespace

/// A name for the scalar this type is stored as.
///
/// I.8.7.1 makes `array-element-compatible-with` "agnostic with respect to
/// enumerations and integral signed-ness", so `int32` and `unsigned int32` name
/// one storage, and so does an enum and the type behind it. Keying on the width
/// merges them, and every other integer of that width with them. A float and an
/// integer of one width stay apart, because no cast between their arrays is valid.
std::string
MethodLLVMEmitter::tbaa_scalar_name (MonoType *t)
{
	MonoType *underlying = mini_get_underlying_type (t);
	int align = 0;
	int size = mono_type_size (underlying, &align);
	bool is_float =
		underlying->type == MONO_TYPE_R4 || underlying->type == MONO_TYPE_R8;

	return std::to_string (size) + (is_float ? "f" : "i");
}

/// The node every fine leaf hangs below, and the leaf a coarse scalar access
/// takes.
llvm::MDNode *
MethodLLVMEmitter::tbaa_coarse_scalar_node ()
{
	llvm::MDBuilder md (context ());

	return md.createTBAANode ("mono managed scalar",
	                          md.createTBAARoot ("mono managed memory"));
}

llvm::MDNode *
MethodLLVMEmitter::tbaa_scalar_node (MonoType *t)
{
	llvm::MDBuilder md (context ());

	return md.createTBAANode ("mono scalar " + tbaa_scalar_name (t),
	                          tbaa_coarse_scalar_node ());
}

/// The descriptor LLVM disambiguates this class's fields with, or null where it
/// cannot have one. statics selects the class's static block, which is a
/// separate allocation from any instance and so gets a descriptor of its own.
///
/// A descriptor asserts that the type is a struct whose members are distinct.
/// LLVM compares the start offsets of two accesses and never checks that their
/// byte ranges are disjoint, so a descriptor whose members overlap makes it
/// answer NoAlias for two accesses that share a byte. The verifier does not
/// catch it: it rejects unsorted members, and an access at an offset the
/// descriptor omits, and it takes an overlap in silence.
///
/// So the gate is whether the fields are provably disjoint, which is the walk
/// that builds the descriptor anyway. II.10.7 lets a `[FieldOffset]` type put
/// two scalars at one offset, and such a type gets no descriptor at all.
llvm::MDNode *
MethodLLVMEmitter::type_descriptor (MonoClass *klass, bool statics)
{
	llvm::DenseMap<MonoClass *, llvm::MDNode *> &cache =
		statics ? static_descriptors : type_descriptors;
	auto cached = cache.find (klass);

	if (cached != cache.end ())
		return cached->second;

	// Claim the slot before walking the fields. A field of this class's own
	// type would otherwise come back in here without end.
	cache [klass] = nullptr;

	std::vector<Member> members;
	gpointer iter = nullptr;

	while (MonoClassField *field = mono_class_get_fields_internal (klass, &iter)) {
		if (((mono_field_get_flags (field) & FIELD_ATTRIBUTE_STATIC) != 0) != statics)
			continue;
		if (mono_field_is_deleted (field))
			continue;

		// A special static lives in a per-thread or per-context block rather
		// than in this one, and its offset is not an offset into any block. Left
		// in, it takes the whole class off the fine leaf.
		if (statics && mono_class_field_is_special_static (field))
			continue;

		MonoType *ftype = mono_field_get_type_internal (field);
		int32_t offset = static_cast<int32_t> (m_field_get_offset (field));

		// field_address () and static_field_address () split the same way.
		if (!statics && m_class_is_valuetype (klass))
			offset -= MONO_ABI_SIZEOF (MonoObject);

		int align = 0;
		int32_t size = static_cast<int32_t> (mono_type_size (ftype, &align));

		if (offset < 0 || size <= 0)
			return nullptr;

		bool named = !mini_type_is_reference (ftype) && !MONO_TYPE_ISSTRUCT (ftype);

		members.push_back ({ offset, size, named, ftype });
	}

	std::sort (members.begin (), members.end (),
	           [] (const Member &a, const Member &b) { return a.offset < b.offset; });

	for (size_t i = 1; i < members.size (); ++i) {
		if (members [i - 1].offset + members [i - 1].size > members [i].offset)
			return nullptr;
	}

	std::vector<std::pair<llvm::MDNode *, uint64_t>> fields;

	for (const Member &member : members) {
		if (!member.named)
			continue;

		fields.emplace_back (tbaa_scalar_node (member.type),
		                     static_cast<uint64_t> (member.offset));
	}

	if (fields.empty ())
		return nullptr;

	/*
	 * The name is the descriptor's identity, because LLVM uniques an MDNode on
	 * its operands. Two assemblies can each declare a type of one full name, so
	 * the assembly goes in front to keep them apart.
	 *
	 * This only has to be good enough. Two classes that reach one name and one
	 * layout share a descriptor, which merges their leaves, and a merged leaf
	 * costs disambiguation rather than correctness. A class can never reach two
	 * names, which is the direction that would be wrong.
	 */
	MonoImage *image = m_class_get_image (klass);
	const char *from = image->assembly_name != nullptr ? image->assembly_name : image->name;
	llvm::MDBuilder md (context ());
	char *full = mono_type_get_full_name (klass);
	std::string name = std::string (from != nullptr ? from : "?") + "!" + full;

	if (statics)
		name += " statics";

	llvm::MDNode *descriptor = md.createTBAAStructTypeNode (name, fields);

	g_free (full);

	cache [klass] = descriptor;

	return descriptor;
}

/**
 * Builds the tag for one managed access, or null where the access carries none.
 *
 * The tree is:
 *
 *     "mono managed memory"                 root
 *     |- "mono managed reference"           every reference access
 *     \- "mono managed scalar"              coarse: an access we cannot place
 *        |- "mono scalar 8f"                the nodes a type descriptor names
 *        \- "mono element 8f[2]"            one leaf per scalar array element
 *
 * Type descriptors sit beside that tree, one for a class's instance fields and
 * one for its static block, naming the "mono scalar" nodes.
 *
 * A field access names its declaring type rather than the receiver's, so an
 * inherited field is one leaf however it is reached. A nested value type never
 * appears in the outer descriptor: only a struct copy names the outer type at
 * those bytes, and a struct copy carries no tag.
 *
 * Every fine node hangs below "mono managed scalar", so an access this declines
 * to place still aliases all of them. That is what lets one opcode carry a fine
 * tag while the next carries none.
 *
 * is_reference must be what mini_type_is_reference () says of the slot, and it
 * outranks the kind: a reference is on its own leaf whatever named it.
 */
llvm::MDNode *
MethodLLVMEmitter::tbaa_tag (const ManagedAccess &access, bool is_reference)
{
	if (access.kind == ManagedAccess::Kind::untagged)
		return nullptr;

	llvm::MDBuilder md (context ());

	if (is_reference) {
		llvm::MDNode *leaf =
			md.createTBAANode ("mono managed reference",
			                   md.createTBAARoot ("mono managed memory"));

		return md.createTBAAStructTagNode (leaf, leaf, 0);
	}

	/*
	 * A shared body names a field's class in its open form, not the
	 * instantiation it runs as, so it cannot say which storage the field
	 * reaches.
	 *
	 * A special static is left out as well. Its block is per thread or per
	 * context. static_field_address () asks an icall for the address, so a
	 * descriptor's offset is not the one the block is laid out by.
	 */
	if (access.kind == ManagedAccess::Kind::field && !depends_on_context (access.field)
	    && !mono_class_field_is_special_static (access.field)) {
		MonoClass *parent = access.field->parent;
		MonoType *ftype = mono_field_get_type_internal (access.field);
		bool statics =
			(mono_field_get_flags (access.field) & FIELD_ATTRIBUTE_STATIC) != 0;

		if (llvm::MDNode *descriptor = type_descriptor (parent, statics)) {
			int32_t offset = static_cast<int32_t> (m_field_get_offset (access.field));

			if (!statics && m_class_is_valuetype (parent))
				offset -= MONO_ABI_SIZEOF (MonoObject);

			return md.createTBAAStructTagNode (descriptor, tbaa_scalar_node (ftype),
			                                   static_cast<uint64_t> (offset));
		}
	}

	// A whole element carries a leaf only where it is one scalar. A value type
	// covers the fields inside it, which are tagged against their own
	// descriptor, and struct-path TBAA cannot name an access that wide. Such an
	// element arrives as a struct copy, and neither emit_memory_store () nor
	// push_from_location () tags one, so this guard is a second line.
	if (access.kind == ManagedAccess::Kind::element
	    && access.element->type != MONO_TYPE_VAR
	    && access.element->type != MONO_TYPE_MVAR
	    && !depends_on_context (mono_class_from_mono_type_internal (access.element))
	    && !MONO_TYPE_ISSTRUCT (mini_get_underlying_type (access.element))) {
		std::string name = "mono element " + tbaa_scalar_name (access.element) + "["
		                   + std::to_string (access.rank) + "]";
		llvm::MDNode *leaf = md.createTBAANode (name, tbaa_coarse_scalar_node ());

		return md.createTBAAStructTagNode (leaf, leaf, 0);
	}

	llvm::MDNode *scalar = tbaa_coarse_scalar_node ();

	return md.createTBAAStructTagNode (scalar, scalar, 0);
}

} // namespace mono
