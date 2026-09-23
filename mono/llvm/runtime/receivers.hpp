/**
 * \file
 * \brief Reading back what tier-1 bodies recorded at their dispatch sites.
 */

#ifndef MONO_LLVM_RUNTIME_RECEIVERS_HPP
#define MONO_LLVM_RUNTIME_RECEIVERS_HPP

#include "jit.hpp"

#include <optional>
#include <unordered_map>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace mono {

/// The receivers one tier-2 compile of \p root reads.
class RecordedReceivers {
public:
	RecordedReceivers (MonoDomain *domain, MonoMethod *root) : domain_ (domain), root_ (root) {}

	/// \p root's own record at \p key, or, where root recorded nothing there,
	/// the record of the method \p key names.
	///
	/// The first is the site as root reached it, inlined into root's body. The
	/// second holds every caller's receivers mixed.
	std::optional<ReceiverCounts> at (const ReceiverSiteKey &key);

private:
	std::optional<ReceiverCounts> recorded_by (MonoMethod *body, const ReceiverSiteKey &key);

	MonoDomain *domain_;
	MonoMethod *root_;
	std::unordered_map<MonoMethod *, std::optional<ProfileCounters>> bodies_;
};

} // namespace mono

#endif
