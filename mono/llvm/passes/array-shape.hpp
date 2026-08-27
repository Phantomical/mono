/**
 * \file
 * \brief Lowering the array shape accessors the translator left symbolic.
 *
 * `Array.GetLength ()` and `Array.GetLowerBound ()` are answered from the
 * object for dimension zero, which every array has. Any other dimension needs
 * the rank test the icall makes, so such a site keeps its call.
 *
 * Which one a site is cannot be read where the site is emitted. A caller that
 * writes a literal often reaches the accessor through `Array.GetUpperBound ()`,
 * which forwards its parameter, so the constant arrives when an inliner folds
 * that body in. The translator therefore emits a `mono.array.shape.*` call
 * whatever the dimension is, and this pass reads the dimension once the
 * inliners have run: zero becomes the loads, and anything else becomes the
 * call to the accessor named on the declaration.
 */

#ifndef MONO_LLVM_PASSES_ARRAY_SHAPE_HPP
#define MONO_LLVM_PASSES_ARRAY_SHAPE_HPP

#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/// The name prefix of the shape declarations, the attribute naming which
/// accessor one stands for, and the attribute naming the method a declined
/// site falls back to. Only the translator writes these three.
///
/// A site takes the array, the dimension, and the corlib type token of the
/// exception a null array raises. The accessor takes the first two.
constexpr llvm::StringRef array_shape_prefix = "mono.array.shape.";
constexpr llvm::StringRef array_shape_attribute = "mono-array-shape";
constexpr llvm::StringRef array_shape_target_attribute = "mono-array-shape-target";

constexpr llvm::StringRef array_shape_length = "length";
constexpr llvm::StringRef array_shape_lower_bound = "lower_bound";

/// Rewrites each site in \p f whose dimension is the constant zero into the
/// header reads, and says whether it changed anything.
///
/// A dimension the IL settled reads as a constant straight away. One that
/// arrives through an inlined parameter is a load until SROA has run, which is
/// why the folds take a function's sites up more than once.
bool fold_array_shapes (llvm::Function &f);

/// Rewrites every site left in \p m, erases the declarations, and says whether
/// it changed anything.
///
/// A dimension this cannot read goes back onto the accessor, which makes the
/// rank test the icall makes. So this must run behind every fold: a restored
/// call is one no fold can read again.
bool lower_array_shapes (llvm::Module &m);

} // namespace mono

#endif
