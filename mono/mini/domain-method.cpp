/**
 * \file
 * \brief The per-domain table of MonoDomainMethod records.
 */

#include "domain-method.hpp"
#include "domain-method.h"

#include "mini-runtime.h"

#include <mono/metadata/domain-internals.h>

#include <llvm/ADT/DenseMap.h>

#include <mutex>

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

struct DomainMethodTable {
	std::mutex lock;
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
}

MonoDomainMethod *
domain_method_find (MonoDomain *domain, MonoMethod *method)
{
	DomainMethodTable *table = table_of (domain);

	if (table == nullptr)
		return nullptr;

	std::lock_guard<std::mutex> held (table->lock);
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
	std::lock_guard<std::mutex> held (table->lock);

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

std::unique_ptr<MonoDomainMethod>
domain_method_take (MonoDomain *domain, MonoMethod *method)
{
	DomainMethodTable *table = table_of (domain);

	if (table == nullptr)
		return nullptr;

	std::lock_guard<std::mutex> held (table->lock);
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
mono_domain_method_table_free (MonoDomain *domain)
{
	delete static_cast<mono::DomainMethodTable *> (
		domain_jit_info (domain)->domain_methods);
	domain_jit_info (domain)->domain_methods = NULL;
}
