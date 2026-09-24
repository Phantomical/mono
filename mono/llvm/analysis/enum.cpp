#include "enum.hpp"

#include "operand-class.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

#include <llvm/IR/Value.h>

#include <algorithm>
#include <cstring>

namespace mono {

MonoClass *
enum_class_of (llvm::Value *boxed, const llvm::Function &f, const ConstantValues &values)
{
	std::pair<MonoClass *, bool> held = operand_class (boxed, f, values);

	if (held.first == nullptr || !held.second)
		return nullptr;

	return m_class_is_enumtype (held.first) ? held.first : nullptr;
}

std::optional<EnumScalar>
enum_scalar (MonoClass *klass)
{
	MonoType *base = mono_class_enum_basetype_internal (klass);

	if (base == nullptr)
		return std::nullopt;

	switch (base->type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_U1:
		return EnumScalar { 8, false };
	case MONO_TYPE_I1:
		return EnumScalar { 8, true };
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U2:
		return EnumScalar { 16, false };
	case MONO_TYPE_I2:
		return EnumScalar { 16, true };
	case MONO_TYPE_U4:
		return EnumScalar { 32, false };
	case MONO_TYPE_I4:
		return EnumScalar { 32, true };
	case MONO_TYPE_U8:
	case MONO_TYPE_U:
		return EnumScalar { 64, false };
	case MONO_TYPE_I8:
	case MONO_TYPE_I:
		return EnumScalar { 64, true };
	default:
		return std::nullopt;
	}
}

std::optional<std::vector<uint64_t>>
enum_literal_values (MonoClass *klass, EnumScalar scalar)
{
	std::vector<uint64_t> literals;
	gpointer iter = nullptr;

	while (MonoClassField *field = mono_class_get_fields_internal (klass, &iter)) {
		if (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC) || mono_field_is_deleted (field))
			continue;

		if (!(field->type->attrs & FIELD_ATTRIBUTE_HAS_DEFAULT))
			return std::nullopt;

		MonoTypeEnum def_type;
		const char *blob = mono_class_get_field_default_value (field, &def_type);

		if (blob == nullptr)
			return std::nullopt;

		mono_metadata_decode_blob_size (blob, &blob);

		uint64_t literal = 0;
		memcpy (&literal, blob, scalar.bits / 8);
		literals.push_back (literal);
	}

	std::sort (literals.begin (), literals.end ());
	literals.erase (std::unique (literals.begin (), literals.end ()), literals.end ());
	return literals;
}

} // namespace mono
