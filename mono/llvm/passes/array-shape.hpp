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
#include <llvm/IR/PassManager.h>

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

/// Rewrites a call to a `mono.array.shape.*` declaration into the header reads
/// where the dimension is the constant zero.
///
/// Runs twice in each tier's pipeline. A site the IL already settled is
/// answered by the first run, in front of the simplification that then
/// optimizes the reads. A site whose dimension arrives through an inlined
/// parameter is a load until SROA has run, so it is the second run that reads
/// it as a constant.
///
/// \param finalize  whether a site this run cannot answer goes back onto the
///                  accessor. Only the last run in a pipeline may, since a
///                  restored call is one no later run can answer.
class ArrayShapePass : public llvm::PassInfoMixin<ArrayShapePass> {
public:
	explicit ArrayShapePass (bool finalize) : finalize (finalize) {}

	llvm::PreservedAnalyses run (llvm::Module &m,
	                             llvm::ModuleAnalysisManager &mam);

private:
	bool finalize;
};

} // namespace mono

#endif
