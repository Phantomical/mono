#ifndef __MONO_INTERP_ARENA_HPP__
#define __MONO_INTERP_ARENA_HPP__

/**
 * \file
 * \brief Storage that is released in one step rather than object by object.
 */

#include "glib.h"
#include <mono/metadata/mempool.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace mono::interp {

/// A bump allocator over a MonoMemPool.
///
/// Values it stores must be trivially destructible, because the arena runs
/// no destructors. Destroying it frees the pool.
class Arena {
public:
	Arena () : pool_ (mono_mempool_new ()) {}

	explicit Arena (int initial_size) : pool_ (mono_mempool_new_size (initial_size)) {}

	Arena (Arena &&other) noexcept : pool_ (other.pool_) { other.pool_ = nullptr; }

	Arena &operator= (Arena &&other) noexcept
	{
		std::swap (pool_, other.pool_);
		return *this;
	}

	Arena (const Arena &) = delete;
	Arena &operator= (const Arena &) = delete;

	~Arena ()
	{
		if (pool_ != nullptr)
			mono_mempool_destroy (pool_);
	}

	/// Returns storage that still holds whatever the pool last left there.
	void *alloc (std::size_t size, std::size_t align)
	{
		if (align <= pool_alignment)
			return mono_mempool_alloc (pool_, (unsigned int) size);

		void *mem = mono_mempool_alloc (pool_, (unsigned int) (size + align - 1));
		return align_up (mem, align);
	}

	/// Returns storage filled with zero.
	void *alloc0 (std::size_t size, std::size_t align)
	{
		if (align <= pool_alignment)
			return mono_mempool_alloc0 (pool_, (unsigned int) size);

		void *mem = mono_mempool_alloc0 (pool_, (unsigned int) (size + align - 1));
		return align_up (mem, align);
	}

	/// Constructs one T from args, or value-initializes it with no arguments,
	/// which zero fills an aggregate.
	template<class T, class... Args>
	T *create (Args &&...args)
	{
		static_assert (std::is_trivially_destructible_v<T>, "an arena runs no destructors");

		return new (alloc (sizeof (T), alignof (T))) T (std::forward<Args> (args)...);
	}

	/// Creates an array of count objects, zero filled.
	template<class T>
	T *create_array (std::size_t count)
	{
		static_assert (std::is_trivially_destructible_v<T>, "an arena runs no destructors");
		static_assert (std::is_trivially_default_constructible_v<T>,
		               "the array is zero filled rather than constructed");

		return (T *) alloc0 (count * sizeof (T), alignof (T));
	}

	/// Creates one T with extra bytes of room after it, for a trailing
	/// flexible-array member. The memory is zero filled.
	template<class T>
	T *create_flexible (std::size_t extra)
	{
		static_assert (std::is_trivially_destructible_v<T>, "an arena runs no destructors");
		static_assert (std::is_trivially_default_constructible_v<T>,
		               "the object is zero filled rather than constructed");

		return (T *) alloc0 (sizeof (T) + extra, alignof (T));
	}

	/// Returns the underlying MonoMemPool, for the runtime's own
	/// mempool-taking helpers.
	MonoMemPool *pool () { return pool_; }

private:
	// What mono_mempool_alloc () lines its results up to - MEM_ALIGN in
	// mempool.c. A stricter request is met by hand, above.
	static constexpr std::size_t pool_alignment = 8;

	static void *align_up (void *mem, std::size_t align)
	{
		std::uintptr_t addr = (std::uintptr_t) mem;

		return (void *) ((addr + align - 1) & ~(std::uintptr_t) (align - 1));
	}

	MonoMemPool *pool_;
};

} // namespace mono::interp

#endif /* __MONO_INTERP_ARENA_HPP__ */
