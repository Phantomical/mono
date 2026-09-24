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

/// The enum class \p boxed is an instance of. Null where \p boxed can be null,
/// its class is unsettled or only bounded, or the class is not an enum.
MonoClass *enum_class_of (llvm::Value *boxed, const llvm::Function &f,
                          const ConstantValues &values);

/// The integer an enum's value is held as.
struct EnumScalar {
	unsigned bits;
	bool is_signed;
};

/// How \p klass holds its value. Nothing for an enum over a floating-point
/// type, which the loader admits and ECMA-335 II.14.3 does not.
std::optional<EnumScalar> enum_scalar (MonoClass *klass);

} // namespace mono

#endif
