#ifndef MONO_LLVM_UTIL_LOCK_HPP
#define MONO_LLVM_UTIL_LOCK_HPP

#include <mutex>

namespace mono {

template<typename M>
class lock_state {
private:
	std::lock_guard<M> guard_;
	bool live_;

public:
	lock_state (M &mutex) : guard_ (mutex), live_ (true) {}

	bool cont () const { return live_; }
	void tick () { live_ = false; }
};

template<typename M>
lock_state<M>
make_lock_state (M &mutex)
{
	return lock_state<M> (mutex);
}

#define MONO_LOCK_CONCAT_IMPL(a, b) a##b
#define MONO_LOCK_CONCAT(a, b) MONO_LOCK_CONCAT_IMPL (a, b)
#define MONO_LOCK_IMPL(mtx, var) \
	for (auto var = ::mono::make_lock_state (mtx); var.cont (); var.tick ())

/// An easy wrapper for creating a lock scope block.
///
/// Use it by doing
///
/// MONO_LOCK(mutex) { .. }
///
/// and the lock will be held for the duration of the scope.
#define MONO_LOCK(mtx) MONO_LOCK_IMPL(mtx, MONO_LOCK_CONCAT(_lock_state_, __LINE__))

} // namespace mono

#endif
