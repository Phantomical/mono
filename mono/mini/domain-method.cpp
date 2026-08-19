/**
 * \file
 * \brief The per-domain table of MonoDomainMethod records.
 */

#include "domain-method.hpp"
#include "domain-method.h"

#include "mini-runtime.h"

#include <mono/llvm/runtime.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/object.h>
#include <mono/metadata/tabledefs.h>
#include <mono/utils/mono-error-internals.h>

#include <llvm/ADT/DenseMap.h>

#include <mutex>
#include <shared_mutex>

namespace mono {

namespace {

/// Holds a domain's lock for a scope.
class DomainLock {
public:
	explicit DomainLock (MonoDomain *domain) : domain_ (domain)
	{
		mono_domain_lock (domain_);
	}

	~DomainLock () { mono_domain_unlock (domain_); }

	DomainLock (const DomainLock &) = delete;
	DomainLock &operator= (const DomainLock &) = delete;

private:
	MonoDomain *domain_;
};

/*
 * A lookup takes the lock shared. Every interpreted call that has to name a
 * method reads this table, and those reads must not queue behind each other.
 */
struct DomainMethodTable {
	std::shared_mutex lock;
	llvm::DenseMap<MonoMethod *, std::unique_ptr<MonoDomainMethod>> methods;
};

DomainMethodTable *
table_of (MonoDomain *domain)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);

	return info != nullptr ? static_cast<DomainMethodTable *> (info->domain_methods)
	                       : nullptr;
}

} // namespace

bool
MonoDomainMethod::publish (MonoTier tier, void *code)
{
	std::lock_guard<std::mutex> held (lock_);

	if (tier < tier_.load (std::memory_order_relaxed))
		return false;

	thunk.redirect (code);
	tier_.store (tier, std::memory_order_release);
	return true;
}

bool
MonoDomainMethod::promote ()
{
	/* Native code owns the entry for good, so there is no tier to move to. */
	if (tier_.load (std::memory_order_acquire) == MonoTier::detoured)
		return true;

	int32_t idle = (int32_t) Promotion::idle;

	/*
	 * Whoever wins this is the one that asks. Several counters can run out at
	 * once - one per engine, and more than one thread inside an engine - and an
	 * engine asked twice compiles the method twice.
	 */
	if (!promotion_.compare_exchange_strong (idle, (int32_t) Promotion::queued,
	                                         std::memory_order_acq_rel,
	                                         std::memory_order_acquire))
		return true;

	if (!mono_llvm_jit_request_promotion (method, domain)) {
		promotion_.store ((int32_t) Promotion::idle, std::memory_order_release);
		return false;
	}

	promotion_.store ((int32_t) Promotion::settled, std::memory_order_release);
	return true;
}

void *
MonoDomainMethod::thunk_address () const
{
	std::lock_guard<std::mutex> held (lock_);

	return thunk.code ();
}

Thunk
MonoDomainMethod::take_thunk ()
{
	std::lock_guard<std::mutex> held (lock_);

	return thunk;
}

void
MonoDomainMethod::attach_body (MonoTier tier, void *code, MonoJitInfo *jinfo)
{
	std::lock_guard<std::mutex> held (lock_);

	if (!bodies_.empty ()) {
		if (bodies_.back ().jinfo != nullptr)
			bodies_.back ().state = BodyState::superseded;
		else
			bodies_.pop_back ();
	}

	bodies_.push_back (MonoMethodBody { tier, BodyState::current, code, jinfo });
}

std::optional<MonoMethodBody>
MonoDomainMethod::body () const
{
	std::lock_guard<std::mutex> held (lock_);

	if (bodies_.empty ())
		return std::nullopt;
	return bodies_.back ();
}

void
MonoDomainMethod::foreach_body (llvm::function_ref<void (const MonoMethodBody &)> visit) const
{
	std::lock_guard<std::mutex> held (lock_);

	for (const MonoMethodBody &body : bodies_)
		visit (body);
}

InterpMethod *
MonoDomainMethod::set_interp_method (InterpMethod *imethod)
{
	InterpMethod *held = nullptr;

	if (interp_method_.compare_exchange_strong (held, imethod, std::memory_order_acq_rel,
	                                            std::memory_order_acquire))
		return imethod;

	return held;
}

llvm::Expected<void *>
MonoDomainMethod::interop_entry ()
{
	if (void *ready = interop_entry_.load (std::memory_order_acquire))
		return ready;

	/*
	 * The domain lock outside the record's, because attaching registers jit
	 * info and that takes it. It is recursive, so a mutator already holding it
	 * - mono_class_proxy_vtable is one - arrives here safely.
	 */
	DomainLock domain_lock (domain);
	std::lock_guard<std::mutex> held (lock_);

	if (void *ready = interop_entry_.load (std::memory_order_relaxed))
		return ready;

	if (llvm::Error err = attach_interop_entry (*this))
		return std::move (err);

	return interop_entry_.load (std::memory_order_relaxed);
}

void
MonoDomainMethod::install_detour (void *target)
{
	/*
	 * The thunk only. The unbox and interop entries reach the method through it,
	 * each having done something first - stepping the receiver past the object
	 * header, taking a C call apart - that pointing them at the target would
	 * take away.
	 */
	publish (MonoTier::detoured, target);

	/*
	 * The interpreter settles once whether it calls a method or interprets it,
	 * and an answer taken before this did not know the entry is native. Outside
	 * publish (), which holds the record lock: this reads the domain's table,
	 * and the table's lock is the outer one.
	 */
	if (mono_use_interpreter)
		mini_get_interp_callbacks ()->method_compiled (domain, method);
}

/*
 * Nothing here is ever taken back, so this only rises. It is what keeps the
 * question off the interpreter's call path in a process with no overrides.
 */
std::atomic<uint32_t> overrides_installed { 0 };

bool
any_method_overridden ()
{
	return overrides_installed.load (std::memory_order_relaxed) != 0;
}

MonoMethod *
method_override_for (MonoDomain *domain, MonoMethod *method)
{
	if (!any_method_overridden ())
		return nullptr;

	MonoDomainMethod *dm = domain_method_find (domain, method);

	return dm != nullptr ? dm->override_method () : nullptr;
}

void
MonoDomainMethod::install_override (MonoMethod *replacement, void *target)
{
	/*
	 * Before the entry moves. A caller that reaches the new entry and then asks
	 * the record who is behind it has to be told the replacement, not nothing.
	 */
	override_.store (replacement, std::memory_order_release);
	overrides_installed.fetch_add (1, std::memory_order_relaxed);

	install_detour (target);
}

MonoDomainMethod *
domain_method_find (MonoDomain *domain, MonoMethod *method)
{
	DomainMethodTable *table = table_of (domain);

	if (table == nullptr)
		return nullptr;

	std::shared_lock<std::shared_mutex> held (table->lock);
	auto it = table->methods.find (method);

	return it != table->methods.end () ? it->second.get () : nullptr;
}

llvm::Expected<MonoDomainMethod *>
domain_method_get (MonoDomain *domain, MonoMethod *method)
{
	DomainMethodTable *table = table_of (domain);

	if (table == nullptr)
		return llvm::createStringError (llvm::inconvertibleErrorCode (),
		                                "the domain has no method table");

	/*
	 * Naming the method reads its signature, and a signature that is not cached
	 * yet is parsed from metadata - which loads the classes it names, and takes
	 * the loader lock to do it. Ask for it here, above the locks: a lock held
	 * across the loader lock deadlocks against a thread in class init, which
	 * holds the loader lock and then takes the domain lock to build a vtable.
	 */
	mono_method_signature_internal (method);

	/*
	 * The domain lock is the outermost of the three. A mutator can arrive here
	 * already holding it - mono_class_proxy_vtable compiles a remoting
	 * trampoline under it - and attaching registers jit info, which takes it as
	 * well.
	 */
	DomainLock domain_lock (domain);
	std::unique_lock<std::shared_mutex> held (table->lock);

	auto it = table->methods.find (method);
	if (it != table->methods.end ())
		return it->second.get ();

	auto record = std::make_unique<MonoDomainMethod> (method, domain);

	if (llvm::Error err = attach_method_entries (*record))
		return std::move (err);

	MonoDomainMethod *raw = record.get ();

	table->methods[method] = std::move (record);
	return raw;
}

void
domain_method_foreach (MonoDomain *domain, llvm::function_ref<void (MonoDomainMethod &)> visit)
{
	DomainMethodTable *table = table_of (domain);

	if (table == nullptr)
		return;

	std::shared_lock<std::shared_mutex> held (table->lock);

	for (const auto &entry : table->methods)
		visit (*entry.second);
}

std::unique_ptr<MonoDomainMethod>
domain_method_take (MonoDomain *domain, MonoMethod *method)
{
	DomainMethodTable *table = table_of (domain);

	if (table == nullptr)
		return nullptr;

	std::unique_lock<std::shared_mutex> held (table->lock);
	auto it = table->methods.find (method);

	if (it == table->methods.end ())
		return nullptr;

	std::unique_ptr<MonoDomainMethod> record = std::move (it->second);

	table->methods.erase (it);
	return record;
}

} // namespace mono

void
mono_domain_method_table_init (MonoDomain *domain)
{
	domain_jit_info (domain)->domain_methods = new mono::DomainMethodTable ();
}

void
mono_install_method_detour (MonoMethod *method, MonoDomain *domain, void *target)
{
	llvm::Expected<mono::MonoDomainMethod *> dm = mono::domain_method_get (domain, method);

	if (!dm) {
		/* No record means no published entry, so nothing holds one to detour. */
		llvm::consumeError (dm.takeError ());
		return;
	}

	(*dm)->install_detour (target);
}

void
mono_install_method_override (MonoMethod *method, MonoDomain *domain, MonoMethod *replacement)
{
	ERROR_DECL (error);
	void *target = mono_compile_method_checked (replacement, error);

	if (!is_ok (error)) {
		/* The replacement has no entry, so there is nothing to point at. */
		mono_error_cleanup (error);
		return;
	}

	llvm::Expected<mono::MonoDomainMethod *> dm = mono::domain_method_get (domain, method);

	if (!dm) {
		/* No record means no published entry, so nothing holds one to override. */
		llvm::consumeError (dm.takeError ());
		return;
	}

	/*
	 * The interpreter reads iflags live while it transforms each caller, so this
	 * keeps every caller transformed from here on out of the inlined case. An
	 * instantiation carries its own copy of the bit, so the definition is marked
	 * as well - otherwise the sibling instantiations stay inlinable.
	 */
	method->iflags |= METHOD_IMPL_ATTRIBUTE_NOINLINING;
	if (method->is_inflated)
		((MonoMethodInflated *) method)->declaring->iflags |= METHOD_IMPL_ATTRIBUTE_NOINLINING;

	(*dm)->install_override (replacement, target);
}

mono_bool
mono_promote_method (MonoMethod *method, MonoDomain *domain)
{
	mono::MonoDomainMethod *dm = mono::domain_method_find (domain, method);

	return dm != nullptr && dm->promote ();
}

void
mono_domain_method_table_free (MonoDomain *domain)
{
	delete static_cast<mono::DomainMethodTable *> (
		domain_jit_info (domain)->domain_methods);
	domain_jit_info (domain)->domain_methods = NULL;
}
