#include "enum.hpp"

#include "operand-class.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

#include <llvm/IR/Value.h>

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

} // namespace mono
