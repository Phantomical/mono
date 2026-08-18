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

#include <mono/llvm/method-symbols.hpp>
#include <mono/llvm/stubs.hpp>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Error.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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
/// between tier1 and detoured rather than at the end.
enum class MonoTier : uint8_t {
	/// Published, with no code yet: the stub points at the lazy resolver.
	none = 0,
	interp = 1,
	tier1 = 2,
	/// Native code owns the entry. Nothing outranks this, and nothing takes it
	/// back.
	detoured = 0xFF,
};

/// How far a promotion request for this method has got.
enum class Promotion : int32_t {
	idle = 0,
	queued = 1,
	settled = 2,
};

/// Everything the runtime knows about one method in one domain.
///
/// The atomic reads - the tier, the call count and the interpreter's two slots -
/// take no lock. Everything else takes the record's lock, which is inside the
/// domain lock and the table's, and outside an engine's own.
class MonoDomainMethod {
public:
	MonoDomainMethod (MonoMethod *method, MonoDomain *domain)
	    : method_ (method), domain_ (domain)
	{
	}

	MonoDomainMethod (const MonoDomainMethod &) = delete;
	MonoDomainMethod &operator= (const MonoDomainMethod &) = delete;

	MonoMethod *method () const { return method_; }
	MonoDomain *domain () const { return domain_; }

	/// The symbol \p entry is published under. Empty until the engine has
	/// attached, since the mangling is the engine's and nothing else has one.
	const std::string &name (Entry entry) const { return names_[(unsigned) entry]; }
	void set_name (Entry entry, std::string name)
	{
		names_[(unsigned) entry] = std::move (name);
	}

	/// The tier that owns the entry now.
	MonoTier tier () const { return tier_.load (std::memory_order_acquire); }

	/// How many calls this method takes at its entry tier before it should be
	/// asked for as the next one. Zero means it never promotes.
	int32_t tier_calls () const { return tier_calls_.load (std::memory_order_relaxed); }
	void set_tier_calls (int32_t calls)
	{
		tier_calls_.store (calls, std::memory_order_relaxed);
	}

	/// Redirects every published entry at the address \p code gives for it, but
	/// only when \p tier outranks the tier already published. Answers whether it
	/// did.
	///
	/// This one comparison is what makes promotion monotone and idempotent. It
	/// is also what keeps a compile that finishes late from taking an entry a
	/// higher tier - or a detour - already owns. A refused publication leaves
	/// every entry as it was, so a caller must not redirect anything itself.
	///
	/// \p code answers null for an entry it has no address for, which is left
	/// alone.
	bool publish (MonoTier tier, llvm::function_ref<void *(Entry)> code);

	/// Asks for the method to be run by the next tier up.
	///
	/// Answers false only when the request was refused and nothing will retry
	/// it, which is the caller's signal to count another threshold of calls. A
	/// method already on its way, and one that native code owns, both answer
	/// true: there is nothing left for the caller to do either way.
	bool promote ();

	/// Hands the entry to native code at \p target, for good.
	///
	/// This always succeeds. A patcher that is told no has nothing to fall back
	/// on. A detour that is installed works for every compiled caller.
	void install_detour (void *target);

	/* -- The stubs ------------------------------------------------------- */

	Stub &stub (Entry entry) { return stubs_[(unsigned) entry]; }
	const Stub &stub (Entry entry) const { return stubs_[(unsigned) entry]; }

	/// The re-entry trampoline \p entry was published pointing at, held so it
	/// can be given back once nothing can reach the stub.
	void *&trampoline (Entry entry) { return trampolines_[(unsigned) entry]; }

	/// The jit-info record \p entry's stub was registered under.
	MonoJitInfo *&jinfo (Entry entry) { return jinfos_[(unsigned) entry]; }

	/// The entries this method was actually published under, recorded rather
	/// than recomputed. Working them out again reads the method's signature, and
	/// by the time one is freed its image may be on its way out: a shorter list
	/// at retire than at publish leaves names claimed and blocks uncarved, and
	/// the next method to land there inherits them.
	llvm::ArrayRef<Entry> published () const { return published_; }
	void set_published (llvm::ArrayRef<Entry> entries)
	{
		published_.assign (entries.begin (), entries.end ());
	}

	/* -- Engine state ---------------------------------------------------- */

	/// What the compiling engine hung on this record.
	void *engine_data () const { return engine_data_.get (); }

	/// Hands \p data to the record, which frees it with \p free when it goes.
	void set_engine_data (void *data, void (*free) (void *))
	{
		engine_data_ = { data, free };
	}

	/* -- The interpreter ------------------------------------------------- */

	/// What the interpreter keeps for this method, or null while the
	/// interpreter has not seen it. The record does not own it: an InterpMethod
	/// comes out of the method's own memory and goes when the method does.
	InterpMethod *interp_method () const
	{
		return interp_method_.load (std::memory_order_acquire);
	}

	/// Gives the record \p imethod and answers what the record holds.
	///
	/// The first caller wins. A later one gets that first record back and must
	/// drop its own, so that every thread names the same one.
	InterpMethod *set_interp_method (InterpMethod *imethod);

	/// How a call from outside the interpreter is taken apart for it, or null
	/// while nothing has asked. One layout serves every method with the same
	/// prototype, so the record names one rather than owning it.
	const arch::InterpEntryLayout *interp_layout () const
	{
		return interp_layout_.load (std::memory_order_acquire);
	}

	void set_interp_layout (const arch::InterpEntryLayout *layout)
	{
		interp_layout_.store (layout, std::memory_order_release);
	}

	std::mutex &lock () { return lock_; }

private:
	MonoMethod *method_;
	MonoDomain *domain_;

	std::string names_[3];
	Stub stubs_[3];
	void *trampolines_[3] = { nullptr, nullptr, nullptr };
	MonoJitInfo *jinfos_[3] = { nullptr, nullptr, nullptr };
	llvm::SmallVector<Entry, 3> published_;

	std::atomic<MonoTier> tier_ { MonoTier::none };
	std::atomic<int32_t> tier_calls_ { 0 };
	/* Promotion::idle / queued / settled; an integer because it is CAS'd. */
	std::atomic<int32_t> promotion_ { (int32_t) Promotion::idle };

	std::unique_ptr<void, void (*) (void *)> engine_data_ { nullptr, nullptr };

	std::atomic<InterpMethod *> interp_method_ { nullptr };
	std::atomic<const arch::InterpEntryLayout *> interp_layout_ { nullptr };

	std::mutex lock_;
};

/// Gives \p dm the stubs it is called through, and whatever state the engine
/// keeps behind them.
///
/// The compiling engine defines this, and each record goes through it once. A
/// failure leaves the record unpublished, and nothing keeps it.
llvm::Error attach_method_entries (MonoDomainMethod &dm);

/// The record for \p method in \p domain, or null when nothing has asked for it
/// yet.
MonoDomainMethod *domain_method_find (MonoDomain *domain, MonoMethod *method);

/// The record for \p method in \p domain, built and published on first ask.
///
/// Building one carves the stub the method is called through, so this fails for
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
/// The caller has to retire what the record holds - stubs, symbols, jit infos -
/// before dropping it. Answers null when the domain has no record for it.
std::unique_ptr<MonoDomainMethod> domain_method_take (MonoDomain *domain,
                                                      MonoMethod *method);

} // namespace mono

#endif /* MONO_MINI_DOMAIN_METHOD_HPP */
