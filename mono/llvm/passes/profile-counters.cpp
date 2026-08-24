#include "profile-counters.hpp"
#include "tier-counter.hpp"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/ProfileData/InstrProf.h>

using namespace llvm;

namespace mono {
namespace {

/// The instrumentation put it in a global of its own: the text an on-disk
/// profile's names section holds for this function.
std::string
recorded_name (const InstrProfCntrInstBase &inc, const Function &f)
{
	if (auto *var = dyn_cast<GlobalVariable> (inc.getNameValue ()))
		if (auto *text = dyn_cast_or_null<ConstantDataArray> (var->getInitializer ()))
			return text->getAsString ().str ();

	return getIRPGOFuncName (f);
}

bool
is_profile_global (const GlobalVariable &global)
{
	// The version marker and the section anchors are "__llvm_prof...". The
	// per-function arrays carry the prefixes below.
	StringRef name = global.getName ();

	return name.starts_with ("__llvm_prof")
	       || name.starts_with (getInstrProfCountersVarPrefix ())
	       || name.starts_with (getInstrProfDataVarPrefix ())
	       || name.starts_with (getInstrProfNameVarPrefix ())
	       || name.starts_with (getInstrProfBitmapVarPrefix ());
}

/// The load \p store reads its counter back through, or null when \p store is
/// not the last step of a read-add-write on a counter.
///
/// That is the shape InstrProfilingLoweringPass leaves an increment in
/// whenever InstrProfOptions::Atomic is off, and no pass between it and this
/// one breaks a group up.
LoadInst *
counter_update_load (StoreInst &store)
{
	if (!store.isSimple () || !is_counter_address (store.getPointerOperand ()))
		return nullptr;

	auto *add = dyn_cast<BinaryOperator> (store.getValueOperand ());

	if (add == nullptr || add->getOpcode () != Instruction::Add || !add->hasOneUse ())
		return nullptr;

	for (Value *operand : {add->getOperand (0), add->getOperand (1)}) {
		auto *load = dyn_cast<LoadInst> (operand);

		if (load != nullptr && load->isSimple () && load->hasOneUse ()
		    && load->getPointerOperand () == store.getPointerOperand ())
			return load;
	}

	return nullptr;
}

} // namespace

bool
is_counter_address (const Value *address)
{
	const auto *global = dyn_cast<GlobalVariable> (getUnderlyingObject (address));

	return global != nullptr
	       && global->getName ().starts_with (getInstrProfCountersVarPrefix ());
}

PreservedAnalyses
ProfileAtomicPass::run (Module &m, ModuleAnalysisManager &)
{
	bool changed = false;

	for (Function &f : m) {
		SmallVector<StoreInst *, 16> updates;

		for (Instruction &i : instructions (f))
			if (auto *store = dyn_cast<StoreInst> (&i))
				if (counter_update_load (*store) != nullptr)
					updates.push_back (store);

		for (StoreInst *store : updates) {
			LoadInst *load = counter_update_load (*store);
			auto *add = cast<BinaryOperator> (store->getValueOperand ());
			Value *step = add->getOperand (0) == load ? add->getOperand (1)
			                                         : add->getOperand (0);
			IRBuilder<> builder (store);

			// Monotonic, which is what LLVM gives an increment it
			// wrote as an atomicrmw itself. A count needs no order
			// against any other count.
			builder.CreateAtomicRMW (AtomicRMWInst::Add,
			                         store->getPointerOperand (), step,
			                         MaybeAlign (), AtomicOrdering::Monotonic);

			store->eraseFromParent ();
			add->eraseFromParent ();
			load->eraseFromParent ();
			changed = true;
		}
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

std::vector<ProfileSite> &
profile_sites ()
{
	static thread_local std::vector<ProfileSite> sites;

	return sites;
}

PreservedAnalyses
ProfileSelectPass::run (Module &m, ModuleAnalysisManager &)
{
	for (Function &f : m)
		if (!f.isDeclaration () && !f.hasFnAttribute (tier_counter_attribute))
			f.addFnAttr (Attribute::NoProfile);

	return PreservedAnalyses::all ();
}

PreservedAnalyses
ProfileGatherPass::run (Module &m, ModuleAnalysisManager &)
{
	std::vector<ProfileSite> &sites = profile_sites ();

	for (Function &f : m) {
		for (Instruction &i : instructions (f)) {
			auto *inc = dyn_cast<InstrProfCntrInstBase> (&i);

			if (inc == nullptr)
				continue;

			ProfileSite site;

			site.function = f.getName ().str ();
			site.name = recorded_name (*inc, f);
			site.hash = inc->getHash ()->getZExtValue ();
			site.counters = inc->getNumCounters ()->getZExtValue ();

			sites.push_back (std::move (site));

			// Every increment in a function carries the same three values.
			break;
		}
	}

	return PreservedAnalyses::all ();
}

PreservedAnalyses
ProfileLocalizePass::run (Module &m, ModuleAnalysisManager &)
{
	for (GlobalVariable &global : m.globals ()) {
		// Private linkage on a declaration is invalid IR, so a profile global
		// with no definition keeps its existing linkage.
		if (!is_profile_global (global) || global.isDeclaration ())
			continue;

		global.setComdat (nullptr);
		global.setLinkage (GlobalValue::PrivateLinkage);
	}

	return PreservedAnalyses::all ();
}

namespace {

using llvm::IPVK_Last;

// The per-function record the instrumentation lowering writes into
// `__llvm_prf_data`. It is built from LLVM's own field-list macro, so its
// layout matches the LLVM version this links against, not a copy made here.
// llvm/ProfileData/InstrProfData.inc documents the pattern. IntPtrT is ours
// to name, and it holds a signed distance.
typedef intptr_t IntPtrT;

struct alignas (INSTR_PROF_DATA_ALIGNMENT) ProfileDataRecord {
#define INSTR_PROF_DATA(Type, LLVMType, Name, Initializer) Type Name;
#include <llvm/ProfileData/InstrProfData.inc>
};

} // namespace

std::vector<ProfileArray>
read_profile_arrays (const uint8_t *data, size_t size)
{
	std::vector<ProfileArray> arrays;

	if (data == nullptr || size == 0 || size % sizeof (ProfileDataRecord) != 0)
		return arrays;

	const auto *records = reinterpret_cast<const ProfileDataRecord *> (data);

	for (size_t i = 0; i < size / sizeof (ProfileDataRecord); i++) {
		const ProfileDataRecord &record = records[i];
		ProfileArray array;

		array.name_key = record.NameRef;
		array.hash = record.FuncHash;
		array.count = record.NumCounters;
		// CounterPtr is the distance from the record to its own counters - a
		// label difference, so the link is what filled it in.
		array.counters = reinterpret_cast<const uint64_t *> (
			reinterpret_cast<const char *> (&record) + record.CounterPtr);

		arrays.push_back (array);
	}

	return arrays;
}

uint64_t
profile_name_key (StringRef name)
{
	return IndexedInstrProf::ComputeHash (name);
}

} // namespace mono
