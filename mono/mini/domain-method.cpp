/**
 * \file
 * \brief The per-domain table of MonoDomainMethod records.
 */

#include "domain-method.hpp"
#include "domain-method.h"

#include "mini-runtime.h"

#include <mono/llvm/runtime.h>
#include <mono/metadata/domain-internals.h>

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
MonoDomainMethod::publish (MonoTier tier, llvm::function_ref<void *(Entry)> code)
{
	std::lock_guard<std::mutex> held (lock_);

	if (tier < tier_.load (std::memory_order_relaxed))
		return false;

	for (Entry entry : published_)
		if (void *address = code (entry))
			stub (entry).redirect (address);

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

	if (!mono_llvm_jit_request_promotion (method_, domain_)) {
		promotion_.store ((int32_t) Promotion::idle, std::memory_order_release);
		return false;
	}

	promotion_.store ((int32_t) Promotion::settled, std::memory_order_release);
	return true;
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
MonoDomainMethod::unbox_entry ()
{
	if (void *ready = unbox_entry_.load (std::memory_order_acquire))
		return ready;

	/*
	 * The domain lock outside the record's, because attaching registers jit
	 * info and that takes it. It is recursive, so a mutator already holding it
	 * - mono_class_proxy_vtable is one - arrives here safely.
	 */
	DomainLock domain_lock (domain_);
	std::lock_guard<std::mutex> held (lock_);

	if (void *ready = unbox_entry_.load (std::memory_order_relaxed))
		return ready;

	if (llvm::Error err = attach_unbox_entry (*this))
		return std::move (err);

	return unbox_entry_.load (std::memory_order_relaxed);
}

void
MonoDomainMethod::install_detour (void *target)
{
	/*
	 * The body entry only. The unbox entry steps the receiver past the object
	 * header and then forwards through the body stub. Pointing it at the target
	 * would take that step away.
	 */
	publish (MonoTier::detoured,
	         [target] (Entry entry) { return entry == Entry::body ? target : nullptr; });

	/*
	 * The interpreter settles once whether it calls a method or interprets it,
	 * and an answer taken before this did not know the entry is native. Outside
	 * publish (), which holds the record lock: this reads the domain's table,
	 * and the table's lock is the outer one.
	 */
	if (mono_use_interpreter)
		mini_get_interp_callbacks ()->method_compiled (domain_, method_);
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
