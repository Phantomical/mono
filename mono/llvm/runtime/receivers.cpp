#include "receivers.hpp"

#include "backend.hpp"
#include "domain-method.hpp"

namespace mono {

std::optional<ReceiverCounts>
RecordedReceivers::recorded_by (MonoMethod *body, const ReceiverSiteKey &key)
{
	auto [cached, fresh] = bodies_.try_emplace (body);

	if (fresh) {
		MonoDomainMethod *dm = domain_method_find (domain_, body);

		if (dm != nullptr)
			cached->second = MonoBackend::profile_of (*dm);
	}

	const std::optional<ProfileCounters> &profile = cached->second;

	if (!profile || profile->receivers == nullptr)
		return std::nullopt;

	std::optional<ReceiverCounts> counts;

	// A body that inlined one callee twice holds a record for each copy.
	for (size_t i = 0; i < profile->receiver_keys.size (); i++) {
		if (!(profile->receiver_keys[i] == key))
			continue;
		if (!counts)
			counts.emplace ();
		counts->add (profile->receivers[i]);
	}

	return counts;
}

std::optional<ReceiverCounts>
RecordedReceivers::at (const ReceiverSiteKey &key)
{
	std::optional<ReceiverCounts> counts = recorded_by (root_, key);

	if (counts && counts->total () != 0)
		return counts;

	auto *owner = reinterpret_cast<MonoMethod *> (static_cast<uintptr_t> (key.method));

	return owner != root_ ? recorded_by (owner, key) : counts;
}

} // namespace mono
