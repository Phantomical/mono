/**
 * \file
 * \brief The declarations System.Enum's operations, and the System.Type
 * operations they reach, are written as, and the calls they lower to.
 */

#ifndef MONO_LLVM_PASSES_ENUM_HPP
#define MONO_LLVM_PASSES_ENUM_HPP

#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/*
 * Each declaration takes the operands of the method it stands for, then that
 * method as a last operand, declared the way an ordinary call site declares
 * it:
 *
 *   i8  @mono.enum.hasflag  (ptr this, ptr flag,  ptr fallback)  Enum.HasFlag ()
 *   i32 @mono.enum.hashcode (ptr this,            ptr fallback)  Enum.get_hashcode ()
 *   i32 @mono.enum.compare  (ptr this, ptr other, ptr fallback)  Enum.CompareTo (),
 *                                                                Enum.InternalCompareTo ()
 *   i8  @mono.enum.equals   (ptr this, ptr other, ptr fallback)  ValueType.DefaultEquals ()
 *   i8  @mono.enum.elementtype (ptr this,         ptr fallback)  Enum.InternalGetCorElementType ()
 *
 *   ptr @mono.type.enumunderlying (ptr type,      ptr fallback)  Enum.InternalGetUnderlyingType ()
 *   ptr @mono.type.enumbox        (ptr type, i64, ptr fallback)  Enum.InternalBoxEnum ()
 *   ptr @mono.type.basetype       (ptr type,      ptr fallback)  RuntimeTypeHandle.GetBaseType ()
 *   i8  @mono.type.isgenericvar   (ptr type,      ptr fallback)  RuntimeTypeHandle.IsGenericVariable ()
 *   i32 @mono.type.attributes     (ptr type,      ptr fallback)  RuntimeTypeHandle.GetAttributes ()
 *
 * A `type` operand is a System.RuntimeType, which analysis/type-info.hpp
 * settles where it is a constant.
 *
 * A site the eliminations in passes/builtins.hpp leave standing lowers into a
 * plain call of its fallback.
 */

constexpr llvm::StringRef enum_hasflag_name = "mono.enum.hasflag";
constexpr llvm::StringRef enum_hashcode_name = "mono.enum.hashcode";
constexpr llvm::StringRef enum_compare_name = "mono.enum.compare";
constexpr llvm::StringRef enum_equals_name = "mono.enum.equals";
constexpr llvm::StringRef enum_elementtype_name = "mono.enum.elementtype";
constexpr llvm::StringRef type_enum_underlying_name = "mono.type.enumunderlying";
constexpr llvm::StringRef type_enum_box_name = "mono.type.enumbox";
constexpr llvm::StringRef type_base_name = "mono.type.basetype";
constexpr llvm::StringRef type_is_generic_var_name = "mono.type.isgenericvar";
constexpr llvm::StringRef type_attributes_name = "mono.type.attributes";

/// The declaration named \p name in \p m, created on first use. \p name must be
/// one of the names above.
llvm::Function *enum_builtin_decl (llvm::Module &m, llvm::StringRef name);

/// Rewrites every remaining call to the declarations above into a call of its
/// own fallback operand, erases the declarations, and says whether it changed
/// anything.
bool lower_enum_builtins (llvm::Module &m);

} // namespace mono

#endif
