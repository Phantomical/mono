/**
 * \file
 * \brief Recording the receiver classes a tier-1 dispatch site sees, and
 * handing them to the tier-2 compile of the same IL.
 *
 * A site is named by the IL that wrote it, the method and the offset, rather
 * than by its place in the CFG. Each tier inlines its own set of bodies, so the
 * same dispatch sits in different functions at the two tiers.
 */

#ifndef MONO_LLVM_PASSES_RECEIVER_PROFILE_HPP
#define MONO_LLVM_PASSES_RECEIVER_PROFILE_HPP

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mono {

/// A dispatch site, as the method whose IL holds it and the offset of the call
/// in that IL. method is the subprogram id il-line-table.hpp reads back.
struct ReceiverSiteKey {
	uint64_t method = 0;
	uint32_t il_offset = 0;

	bool operator== (const ReceiverSiteKey &other) const
	{
		return method == other.method && il_offset == other.il_offset;
	}
};

/// One site's record in a tier-1 body. The code and
/// mono_llvm_jit_record_receiver () both write it while the process runs.
///
/// A vtable of zero is an entry nothing has claimed yet. A receiver that finds
/// every entry claimed by another class takes over the entry with the lowest
/// count, count included, and notes that count in inherited: so count minus
/// inherited is what the class itself was seen. A class above a quarter of the
/// receivers therefore cannot be pushed out.
struct ReceiverRecord {
	static constexpr unsigned entries = 4;

	struct Entry {
		std::atomic<uint64_t> vtable;
		std::atomic<uint64_t> count;
		std::atomic<uint64_t> inherited;
	};

	Entry seen[entries];
	/// The byte offset into seen of the entry the lowered code compares
	/// against, kept close to the highest count.
	std::atomic<uint64_t> hot;
};

static_assert (sizeof (ReceiverRecord) == (3 * ReceiverRecord::entries + 1) * sizeof (uint64_t),
               "the pass lays the record out as plain i64s");

/// Where one function's records start in its object's record section, and
/// which site each one is.
struct ReceiverSites {
	/// The function the sites are in, by the name it carries in the IR.
	std::string function;
	/// The index of the function's first record in the section.
	uint32_t first = 0;
	std::vector<ReceiverSiteKey> keys;
};

/// What ReceiverProfilePass recorded for the module this thread last ran the
/// tier-1 pipeline over. The caller empties it before each run.
std::vector<ReceiverSites> &receiver_sites ();

/// The section every record of an object lands in.
constexpr llvm::StringRef receiver_section = ".mono_receivers";

/// A snapshot of what one site or several with the same key recorded.
struct ReceiverCounts {
	/// Each vtable seen, with its count, in no order.
	llvm::SmallVector<std::pair<uint64_t, uint64_t>, 4> seen;
	/// Receivers counted under no vtable in seen.
	uint64_t other = 0;

	/// Adds what \p record holds so far.
	void add (const ReceiverRecord &record);

	uint64_t total () const;
};

/// Gives each dispatch site in an instrumented function a record, and a call
/// that counts the site's receiver into it.
///
/// Run behind the PGO instrumentation, so that the call is not part of the CFG
/// the tier-2 compile matches its counts against.
class ReceiverProfilePass : public llvm::PassInfoMixin<ReceiverProfilePass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

/// Writes each call ReceiverProfilePass left as the code that counts.
class LowerReceiverProfilePass : public llvm::PassInfoMixin<LowerReceiverProfilePass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

/// Puts what `current_compile ().receivers` returns for each dispatch site on
/// the site, as `!prof` value-profile metadata of kind IPVK_VTableTarget.
class AnnotateReceiversPass : public llvm::PassInfoMixin<AnnotateReceiversPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif
