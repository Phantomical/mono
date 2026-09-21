#include "enum-flag.hpp"

#include "operand-class.hpp"

#include "mono/metadata/class-internals.h"

#include <llvm/IR/Value.h>

namespace mono {

MonoClass *
enum_has_flag_class (llvm::Value *receiver, llvm::Value *flag, const llvm::Function &f,
                     const ConstantValues &values)
{
	std::pair<MonoClass *, bool> held_receiver = operand_class (receiver, f, values);

	if (held_receiver.first == nullptr || !held_receiver.second)
		return nullptr;

	std::pair<MonoClass *, bool> held_flag = operand_class (flag, f, values);

	if (!held_flag.second || held_flag.first != held_receiver.first)
		return nullptr;

	return m_class_is_enumtype (held_receiver.first) ? held_receiver.first : nullptr;
}

} // namespace mono
