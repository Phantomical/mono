/**
 * \file
 * \brief Whether an Enum.HasFlag () site's own operands settle its class.
 */

#ifndef MONO_LLVM_ANALYSIS_ENUM_FLAG_HPP
#define MONO_LLVM_ANALYSIS_ENUM_FLAG_HPP

namespace llvm {
class Function;
class Value;
} // namespace llvm

typedef struct _MonoClass MonoClass;

namespace mono {

class ConstantValues;

/// The enum class both \p receiver and \p flag are. Null where either is
/// unsettled, the two disagree, or the class is not an enum.
MonoClass *enum_has_flag_class (llvm::Value *receiver, llvm::Value *flag,
                                const llvm::Function &f, const ConstantValues &values);

} // namespace mono

#endif
