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

#include <mono/utils/mono-lock-rank.h>

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

namespace mono {

/// Which engine owns the address a method is entered at.
///
/// The order is the ranking publish () compares, so a tier added later goes
/// between tier2 and detoured rather than at the end.
enum class MonoTier : uint8_t {
	/// Published, with no code yet: the thunk points at the lazy resolver.
	none = 0,
	/// Native code from the classic compiler (mono/mini/tier0/), which is
	/// where a method starts.
	tier0 = 2,
	tier1 = 3,
	tier2 = 4,
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
	case MonoTier::tier0:
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
struct MonoMethodBody {
	MonoTier tier = MonoTier::none;
	BodyState state = BodyState::current;
	void *code = nullptr;
	MonoJitInfo *jinfo = nullptr;
};

using RecordLock = MonoRankedMutex<std::mutex, MONO_LOCK_RANK_DOMAIN_METHOD>;

/// Everything the runtime knows about one method in one domain.
///
/// The atomic reads - the tier and the call count - take no lock. Everything
/// else takes the record's lock.
class MonoDomainMethod {
public:
	MonoDomainMethod (MonoMethod *method, MonoDomain *domain) : method (method), domain (domain) {}

	MonoDomainMethod (const MonoDomainMethod &) = delete;
	MonoDomainMethod &operator= (const MonoDomainMethod &) = delete;

	/// What the record is for. Fixed when it is built.
	MonoMethod *const method;
	MonoDomain *const domain;

	/// The symbol the method's thunk is published under. method_stub_symbol ()
	/// produces it, and the table sets it before attaching the record.
	std::string name;

	/// Whether the signature passes a value in a register wider than 128 bits.
	/// Compute this before taking the domain lock to preserve lock ordering.
	bool needs_wide_vector_register = false;

	/// Whether native code can call this method directly, cached at interning.
	bool exposed_to_native_code = false;

	/// The tier that owns the entry now.
	MonoTier tier () const { return tier_.load (std::memory_order_acquire); }

	/// What this method's counter at its entry tier starts at, in the units
	/// mono_llvm_jit_tier0_budget () answers in. Zero means it never promotes.
	std::atomic<int32_t> tier_budget{0};

	/// Classic tier0's own live count of what remains before it asks for the
	/// next tier. A call takes mono_llvm_jit_tier0_entry_weight () off it and a
	/// loop's own back edge takes the loop's IL bytes. Armed from tier_budget.
	std::atomic<int32_t> tier0_counter{0};

	/// Redirects the method's entry at \p code, but only when \p tier outranks
	/// the tier already published. Returns whether it did.
	///
	/// This one comparison is what makes promotion monotone and idempotent. It
	/// is also what keeps a compile that finishes late from taking an entry a
	/// higher tier - or a detour - already owns. A refused publication leaves
	/// the entry as it was, so a caller must not redirect anything itself.
	///
	/// Pass \p epoch for a body a compile produced: the value inlines_epoch ()
	/// gave when that compile started. Such a body is refused whatever its tier
	/// once the epoch has moved, since a method it holds a copy of has been
	/// replaced. Leave it out for code that inlines nothing in.
	bool publish (MonoTier tier, void *code, std::optional<uint32_t> epoch = std::nullopt);

	/// Counts the times a method this one inlined has been replaced.
	///
	/// Read this before a compile translates the method, and hand it back to
	/// publish (). A compile that spans a replacement built its body from IL
	/// that is gone.
	uint32_t inlines_epoch () const { return inlines_epoch_.load (std::memory_order_acquire); }

	/// Whether tier 0 is closed to this method, having already run compiled.
	bool past_tier0 () const { return past_tier0_.load (std::memory_order_acquire); }

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
	void note_inlined_into (MonoMethod *root);

	/// Takes the entry of every method that inlined a copy of this one in back
	/// to its lazy resolver. The next call to one of them compiles it again,
	/// and is_inlinable () keeps the copy out that time.
	///
	/// A thread already inside such a body stays there, since there is no
	/// on-stack replacement here. So this decides what later calls enter rather
	/// than what is executing.
	void drop_inlined_bodies ();

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

	/// The entry if one has been published, without compiling one.
	void *interop_entry_if_ready () const
	{
		return interop_entry_.load (std::memory_order_acquire);
	}

	/// Publishes \p code as the entry, and returns what callers will reach -
	/// which is another thread's body where one got here first. Two threads
	/// build one each only when neither could wait for the other, and the
	/// loser's is superseded rather than freed.
	void *set_interop_entry (void *code)
	{
		void *first = nullptr;

		if (interop_entry_.compare_exchange_strong (first, code,
		                                            std::memory_order_release,
		                                            std::memory_order_acquire))
			return code;

		return first;
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

	/// The re-entry trampoline that compiles the method, which the one above
	/// sends it to. Held for the same reason, and rearmed beside it: a call
	/// can be on its way here while the entry moves.
	void *compile_trampoline = nullptr;

	/// The jit-info record the thunk was registered under.
	MonoJitInfo *jinfo = nullptr;

	/* -- The bodies ------------------------------------------------------ */

	/// Records \p code as the body the entry names.
	///
	/// Whatever it replaces becomes superseded, unless nothing can name a frame
	/// in it - which is what a body with no jit info means - in which case it
	/// goes.
	void attach_body (MonoTier tier, void *code, MonoJitInfo *jinfo);

	/// The body the entry names, or nothing while none does.
	///
	/// Answered by value. A compile on another thread can supersede the body
	/// between the read and the use, and the caller wants what was current when
	/// it asked. A method whose entry went back to its lazy resolver has no such
	/// body, however many it has run.
	std::optional<MonoMethodBody> body () const;

	/// Calls \p visit with every body the method has, oldest first, with the
	/// record locked.
	void foreach_body (llvm::function_ref<void (const MonoMethodBody &)> visit) const;

	/* -- Engine state ---------------------------------------------------- */

	/// What the compiling engine hung on this record, freed with the record.
	std::unique_ptr<void, void (*) (void *)> engine_data{nullptr, nullptr};

private:
	std::atomic<void *> interop_entry_ { nullptr };

	llvm::SmallVector<MonoMethodBody, 2> bodies_;

	std::atomic<bool> override_checked_ { false };

	/// The methods whose compiled bodies hold a copy of this one's.
	llvm::SmallVector<MonoMethod *, 2> inlined_into_;
	/// How many methods this one holds a copy of have been replaced.
	std::atomic<uint32_t> inlines_epoch_ { 0 };
	std::atomic<bool> past_tier0_ { false };

	/// Takes the entry back to the lazy resolver, so the next call compiles the
	/// method again.
	void unwind_inlined_body ();

	std::atomic<MonoTier> tier_ { MonoTier::none };
	/* The highest tier anything has asked for, which is what keeps two requests
	 * arriving at once from queueing the same compile twice. */
	std::atomic<MonoTier> requested_ { MonoTier::none };

	mutable RecordLock lock_;
};

/// Gives \p dm the thunk it is called through, and whatever state the engine
/// keeps behind it.
///
/// The compiling engine defines this, and each record goes through it once. A
/// failure leaves the record unpublished, and nothing keeps it. Whatever \p dm
/// needs the loader lock for is settled before it arrives, because attaching
/// runs under the domain lock.
llvm::Error attach_method_entries (MonoDomainMethod &dm);

/// The symbol \p method's thunk is published under.
///
/// The compiling engine defines this, since only it has a mangling. Call it
/// with the domain lock unheld: naming describes the method's signature, and a
/// description resolves the classes a custom modifier names, which takes the
/// loader lock.
std::string method_stub_symbol (MonoMethod *method);

/// Returns whether \p method passes a Vector256<T> parameter or return value.
/// Call this without the domain lock because reading the signature can acquire
/// the loader lock.
bool method_needs_wide_vector_register (MonoMethod *method);

/// Returns whether native code can call \p method directly.
bool method_is_exposed_to_native_code (MonoMethod *method);

/// What \p method's tier-0 counter starts at, or 0 for a method that does not
/// run at tier 0.
///
/// The compiling engine defines this, since the tier policy is its own. Call it
/// with the domain lock unheld, for the same reason method_stub_symbol () is:
/// deciding can name the method.
int32_t method_tier0_budget (MonoMethod *method);

/// Gives \p dm the entry native code enters the method through.
///
/// The compiling engine defines this. Called with no lock held: it compiles,
/// which takes the loader lock, and registering the jit info takes the domain
/// lock. A method nothing native enters is left with none and is not an error.
llvm::Error attach_interop_entry (MonoDomainMethod &dm);

/// The address that stands for \p dm's method wherever one is handed out: an
/// ldftn, a stub request, or a raw function pointer.
///
/// The compiling engine defines this. Call it with no lock on \p dm held. It
/// can compile, and what it compiles can inline this method in, which wants the
/// record's own lock.
llvm::Expected<void *> published_entry_of (MonoDomainMethod &dm);

/// Returns a published entry without compiling or publishing a record.
/// Returns null when the method is not ready for the fast path.
void *published_entry_if_ready (MonoDomain *domain, MonoMethod *method);

/// The record for \p method in \p domain, or null when nothing has asked for it
/// yet.
MonoDomainMethod *domain_method_find (MonoDomain *domain, MonoMethod *method);

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
