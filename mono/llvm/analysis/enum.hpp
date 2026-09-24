/**
 * \file
 * \brief What a boxed enum's own IR settles about its class and its value.
 */

#ifndef MONO_LLVM_ANALYSIS_ENUM_HPP
#define MONO_LLVM_ANALYSIS_ENUM_HPP

#include <optional>

namespace llvm {
class Function;
class Value;
} // namespace llvm

typedef struct _MonoClass MonoClass;

namespace mono {

class ConstantValues;

/// Returns the exact enum class of \p boxed, or null unless it is known to be a
/// non-null enum instance.
MonoClass *enum_class_of (llvm::Value *boxed, const llvm::Function &f,
                          const ConstantValues &values);

/// The width and signedness of an enum's underlying value.
struct EnumScalar {
	unsigned bits;
	bool is_signed;
};

/// Returns the representation of \p klass, or nothing for a floating-point
/// enum accepted by the loader but disallowed by ECMA-335 II.14.3.
std::optional<EnumScalar> enum_scalar (MonoClass *klass);

} // namespace mono

#endif
