/**
 * \file
 * \brief One record per (domain, method), owned by mono rather than by an engine.
 *
 * The address callers enter a method at, the tier that owns that address, and
 * the code behind it are one fact in one place. That is what makes asking what
 * a method is - or freeing it - a matter of this record rather than of every
 * table it was written into.
 *
 * An engine attaches its own state to a record and never owns its lifetime.
 */

#ifndef MONO_MINI_DOMAIN_METHOD_HPP
#define MONO_MINI_DOMAIN_METHOD_HPP

#include "thunk.hpp"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Error.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoJitInfo MonoJitInfo;
typedef struct _MonoMethod MonoMethod;

/* The interpreter's own record for a method. Only mono/interp reads it. */
struct InterpMethod;

namespace mono {

namespace arch {
struct InterpEntryLayout;
}

/// Which engine owns the address a method is entered at.
///
/// The order is the ranking publish () compares, so a tier added later goes
/// between tier2 and detoured rather than at the end.
enum class MonoTier : uint8_t {
	/// Published, with no code yet: the thunk points at the lazy resolver.
	none = 0,
	interp = 1,
	tier1 = 2,
	tier2 = 3,
	/// Native code owns the entry. Nothing outranks this, and nothing takes it
	/// back.
	detoured = 0xFF,
};

/// The tier a method at \p tier promotes to, or \p tier itself at the top.
constexpr MonoTier
next_tier (MonoTier tier)
{
	switch (tier) {
	case MonoTier::none:
	case MonoTier::interp:
		return MonoTier::tier1;
	case MonoTier::tier1:
		return MonoTier::tier2;
	default:
		return tier;
	}
}

/// How far a body has got from being the one the entry names.
enum class BodyState : uint8_t {
	current,
	/// A later compile took the entry. A thread already running in this code
	/// still is, so it stays allocated and its jit info stays in the table.
	superseded,
};

/// One compile of a method: where the code is, and the record something walking
/// the stack reads to name a frame in it.
///
/// A body at tier 0 is the shared interpreter entry, and its record is null:
/// every interpreted method enters through that code, so no frame in it belongs
/// to this method rather than another.
struct MonoMethodBody {
	MonoTier tier = MonoTier::none;
	BodyState state = BodyState::current;
	void *code = nullptr;
	MonoJitInfo *jinfo = nullptr;
};

/// Everything the runtime knows about one method in one domain.
///
/// The atomic reads - the tier, the call count and the interpreter's two slots -
/// take no lock. Everything else takes the record's lock, which is inside the
/// domain lock and the table's, and outside an engine's own.
class MonoDomainMethod {
public:
	MonoDomainMethod (MonoMethod *method, MonoDomain *domain) : method (method), domain (domain) {}

	MonoDomainMethod (const MonoDomainMethod &) = delete;
	MonoDomainMethod &operator= (const MonoDomainMethod &) = delete;

	/// What the record is for. Fixed when it is built.
	MonoMethod *const method;
	MonoDomain *const domain;

	/// The symbol the method's thunk is published under. Empty until the engine
	/// has attached, since only the engine has a mangling.
	std::string name;

	/// The tier that owns the entry now.
	MonoTier tier () const { return tier_.load (std::memory_order_acquire); }

	/// How many calls this method takes at its entry tier before it is asked
	/// for as the next one. Zero means it never promotes.
	std::atomic<int32_t> tier_calls{0};

	/// Redirects the method's entry at \p code, but only when \p tier outranks
	/// the tier already published. Returns whether it did.
	///
	/// This one comparison is what makes promotion monotone and idempotent. It
	/// is also what keeps a compile that finishes late from taking an entry a
	/// higher tier - or a detour - already owns. A refused publication leaves
	/// the entry as it was, so a caller must not redirect anything itself.
	///
	/// Tier 2 is refused outright once drop_folded_bodies () has run, which is
	/// what stops a compile already in flight from putting a stale copy back.
	bool publish (MonoTier tier, void *code);

	/// Asks for the method to be run by the next tier up.
	///
	/// Which tier that is comes from the one running the method now, so the
	/// counter a tier arms decides nothing beyond when to call this.
	///
	/// Returns false only when the request was refused and nothing will retry
	/// it, which is the caller's signal to count another threshold of calls. A
	/// method already on its way, one at the top tier, and one that native code
	/// owns all return true: there is nothing left for the caller to do.
	bool promote ();

	/// Hands the entry to native code at \p target, for good.
	///
	/// This always succeeds. A patcher that is told no has nothing to fall back
	/// on. A detour that is installed works for every compiled caller.
	void install_detour (void *target);

	/// Makes \p replacement the body of this method, entered at \p target.
	///
	/// Every caller reaches \p replacement afterwards, whichever engine it runs
	/// in. This always succeeds, and a later override replaces it.
	void install_override (MonoMethod *replacement, void *target);

	/// Records that \p root's compiled body holds a copy of this method's.
	///
	/// A copy sits under no thunk, so redirecting this method's entry does not
	/// reach it. This is how a detour finds the bodies it has to take down.
	void note_folded_into (MonoMethod *root);

	/// Takes the entry of every method that folded a copy of this one in back to
	/// the newest body below tier 2, and bars them from tier 2 for good.
	///
	/// A thread already inside such a body stays there, since there is no
	/// on-stack replacement here. So this decides what later calls enter rather
	/// than what is executing. The bar is what keeps which body runs from
	/// depending on how a patcher and a compile worker interleaved.
	void drop_folded_bodies ();

	/// The method standing in for this one, or null while none does.
	///
	/// The interpreter reads this to decide whose bytecode a call runs. A
	/// detour to plain native code leaves it null: there is no managed method
	/// behind that entry to name.
	MonoMethod *override_method () const
	{
		return override_.load (std::memory_order_acquire);
	}

	/// Takes the right to ask the override table about this method.
	///
	/// Returns true to the first caller and false to every other, so the table
	/// is read once per record however many threads arrive at it.
	bool claim_override_check ()
	{
		return !override_checked_.exchange (true, std::memory_order_acq_rel);
	}

	/* -- The thunks -------------------------------------------------------- */

	/// The entry a call off a value type's vtable or IMT arrives at.
	///
	/// It steps the receiver past the object header and runs into the body
	/// entry, so whatever redirects the method redirects this too and no tier
	/// ever has to rewrite it. Every method has one, used or not. Hand it out
	/// only for a method the engine accepts: a receiver with no object header in
	/// front of it is stepped past bytes that are not there.
	void *unbox_entry () const { return thunk ? thunk.unbox () : nullptr; }

	/// The C-convention entry native code enters the method through, compiled
	/// on first ask. Null for a method nothing native enters.
	///
	/// It calls through the method's thunk, so whatever redirects the method
	/// redirects this too and no tier ever has to rebuild it.
	llvm::Expected<void *> interop_entry ();

	void set_interop_entry (void *code)
	{
		interop_entry_.store (code, std::memory_order_release);
	}

	/// The one entry every caller reaches the method at.
	Thunk thunk;

	/// The thunk's address, read under the record's own lock.
	///
	/// A caller that publishes a fresh reference to this method - resolving a
	/// call target against this record, say - reads this while the record is
	/// still the one answering for the method. take_thunk () takes the same
	/// lock at retire, so the two can never interleave: whichever runs first is
	/// what the other sees.
	void *thunk_address () const;

	/// Hands back the record's thunk, under the same lock thunk_address ()
	/// reads under, for the caller to quarantine. Called once, at retire.
	Thunk take_thunk ();

	/// The re-entry trampoline the thunk was published pointing at, held so it
	/// can be given back once nothing can reach the thunk.
	void *trampoline = nullptr;

	/// The jit-info record the thunk was registered under.
	MonoJitInfo *jinfo = nullptr;

	/* -- The bodies ------------------------------------------------------ */

	/// Records \p code as the body the entry names.
	///
	/// Whatever it replaces becomes superseded, unless nothing can name a frame
	/// in it - which is what a body with no jit info means - in which case it
	/// goes.
	void attach_body (MonoTier tier, void *code, MonoJitInfo *jinfo);

	/// The body the entry names, or nothing while none has been compiled.
	///
	/// Answered by value. A compile on another thread can supersede the body
	/// between the read and the use, and the caller wants what was current when
	/// it asked.
	std::optional<MonoMethodBody> body () const;

	/// Calls \p visit with every body the method has, oldest first, with the
	/// record locked.
	void foreach_body (llvm::function_ref<void (const MonoMethodBody &)> visit) const;

	/* -- Engine state ---------------------------------------------------- */

	/// What the compiling engine hung on this record, freed with the record.
	std::unique_ptr<void, void (*) (void *)> engine_data{nullptr, nullptr};

	/* -- The interpreter ------------------------------------------------- */

	/// What the interpreter keeps for this method, or null while the
	/// interpreter has not seen it. The record does not own it: an InterpMethod
	/// comes out of the method's own memory and goes when the method does.
	InterpMethod *interp_method () const
	{
		return interp_method_.load (std::memory_order_acquire);
	}

	/// Gives the record \p imethod and returns what the record holds.
	///
	/// The first caller wins. A later one gets that first record back and must
	/// drop its own, so that every thread names the same one.
	InterpMethod *set_interp_method (InterpMethod *imethod);

	/// How a call from outside the interpreter is taken apart for it, or null
	/// while nothing has asked. One layout serves every method with the same
	/// prototype, so the record names one rather than owning it.
	std::atomic<const arch::InterpEntryLayout *> interp_layout{nullptr};

private:
	std::atomic<void *> interop_entry_ { nullptr };

	llvm::SmallVector<MonoMethodBody, 2> bodies_;

	std::atomic<MonoMethod *> override_ { nullptr };
	std::atomic<bool> override_checked_ { false };

	/// The methods whose compiled bodies hold a copy of this one's.
	llvm::SmallVector<MonoMethod *, 2> folded_into_;
	/// Whether a body of this method folded in a method that has since been
	/// replaced, which is what bars tier 2.
	std::atomic<bool> folds_stale_ { false };

	/// Puts the entry back on the newest body below tier 2, and returns whether
	/// there was one to go back to.
	bool unwind_to_earlier_tier ();

	std::atomic<MonoTier> tier_ { MonoTier::none };
	/* The highest tier anything has asked for, which is what keeps two counters
	 * running out at once from queueing the same compile twice. */
	std::atomic<MonoTier> requested_ { MonoTier::none };

	std::atomic<InterpMethod *> interp_method_ { nullptr };

	mutable std::mutex lock_;
};

/// Gives \p dm the thunk it is called through, and whatever state the engine
/// keeps behind it.
///
/// The compiling engine defines this, and each record goes through it once. A
/// failure leaves the record unpublished, and nothing keeps it.
llvm::Error attach_method_entries (MonoDomainMethod &dm);

/// Gives \p dm the entry native code enters the method through.
///
/// The compiling engine defines this. Called with \p dm's lock held, and with
/// the domain's, since registering the jit info takes it. A method nothing
/// native enters is left with none and is not an error.
llvm::Error attach_interop_entry (MonoDomainMethod &dm);

/// The record for \p method in \p domain, or null when nothing has asked for it
/// yet.
MonoDomainMethod *domain_method_find (MonoDomain *domain, MonoMethod *method);

/// Whether any method anywhere has been overridden.
///
/// Every interpreted method asks whether it is overridden, so the answer has to
/// be cheap in the ordinary case where nothing is.
bool any_method_overridden ();

/// The method standing in for \p method in \p domain, or null when none does.
///
/// Carves no thunk and builds no record, so it is cheap enough to ask at every
/// call site. One hop: where the replacement is itself overridden, this still
/// returns the replacement.
MonoMethod *method_override_for (MonoDomain *domain, MonoMethod *method);

/// The record for \p method in \p domain, built and published on first ask.
///
/// Building one carves the thunk the method is called through, so this fails for
/// the same reasons publishing does. Two threads asking at once get the same
/// record.
llvm::Expected<MonoDomainMethod *> domain_method_get (MonoDomain *domain,
                                                     MonoMethod *method);

/// Calls \p visit with every record \p domain holds.
///
/// The table stays locked for the walk, so \p visit must not ask this domain
/// for another record.
void domain_method_foreach (MonoDomain *domain,
                            llvm::function_ref<void (MonoDomainMethod &)> visit);

/// Takes \p method's record out of \p domain and hands it over.
///
/// The caller has to retire what the record holds - thunks, symbols, jit infos -
/// before dropping it. Returns null when the domain has no record for it.
std::unique_ptr<MonoDomainMethod> domain_method_take (MonoDomain *domain,
                                                      MonoMethod *method);

} // namespace mono

#endif /* MONO_MINI_DOMAIN_METHOD_HPP */
