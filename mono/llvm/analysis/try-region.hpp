/**
 * \file
 * \brief Which IL try region a machine instruction's location lies in.
 */

#ifndef MONO_LLVM_ANALYSIS_TRY_REGION_HPP
#define MONO_LLVM_ANALYSIS_TRY_REGION_HPP

#include <cstdint>
#include <vector>

namespace llvm {
class DILocation;
class Function;
} // namespace llvm

namespace mono {

/// The try regions recorded by the front end in `!mono.clauses`.
class TryRegions {
public:
	explicit TryRegions (const llvm::Function &f);

	/// The index of the innermost clause whose try region holds \p il, or -1.
	///
	/// Equal-length regions use the first clause, matching the EH pad selection.
	int innermost (int il) const;

	/// Returns the function IL offset represented by \p loc, or -1 if it is null.
	///
	/// For inlined code, the outermost location names the call site.
	static int il_offset (const llvm::DILocation *loc);

private:
	struct Clause {
		int index;
		std::uint32_t try_offset;
		std::uint32_t try_len;
	};

	std::vector<Clause> clauses_;
};

} // namespace mono

#endif /* MONO_LLVM_ANALYSIS_TRY_REGION_HPP */
