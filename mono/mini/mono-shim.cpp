/**
 * \file
 * \brief The allocation surface a Unity player build defines for itself.
 */

#include <config.h>

#include <glib.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>

/*
 * A Unity player defines the process's global operator new, so any shape this
 * image leaves undefined binds there and allocates on the engine heap.  These
 * definitions and -Wl,-Bsymbolic together keep that traffic on g_malloc, which
 * is also the seam an embedder's MonoAllocatorVTable reaches.
 *
 * Define the whole family.  A shape left out binds to the player's definition
 * while its partner stays here.  One heap then frees what the other allocated.
 *
 * g_malloc calls g_error rather than answering null, so the throwing forms
 * abort where the standard has them raise std::bad_alloc.  The runtime treats
 * a failed allocation as fatal everywhere else too.
 */

namespace {

// g_malloc answers null for a zero-size request. operator new owes a unique
// pointer instead.
gsize
at_least_one (std::size_t size)
{
	return size != 0 ? size : 1;
}

// glib.h spells g_malloc as a macro that casts its result, so the name cannot
// be taken as a function pointer. These are what aligned_new is handed.
gpointer
alloc_or_die (gsize size)
{
	return g_malloc (size);
}

gpointer
alloc_or_null (gsize size)
{
	return g_try_malloc (size);
}

// What g_malloc answers with, and so what an embedder's MonoAllocatorVTable
// has to answer with too. An aligned request inside this needs nothing further.
constexpr std::size_t allocator_align = 16;

// The plain operator new below hands back whatever the allocator gave it, so a
// vtable that promises less than the compiler expects under-aligns every
// allocation the aligned forms never see.
static_assert (allocator_align >= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
               "the allocator owes at least what a plain new does");

// Past that bound, carry what g_malloc returned in the word below the address
// handed out. Both halves branch on the same align, so a pointer reaches the
// free that matches how it was made.
void *
aligned_new (std::size_t size, std::size_t align, gpointer (*alloc) (gsize))
{
	if (align <= allocator_align)
		return alloc (at_least_one (size));

	auto base = static_cast<char *> (alloc (at_least_one (size) + align
	                                        + sizeof (void *) - 1));

	if (base == nullptr)
		return nullptr;

	auto raw = reinterpret_cast<std::uintptr_t> (base) + sizeof (void *);
	auto out = reinterpret_cast<char *> ((raw + align - 1)
	                                     & ~(static_cast<std::uintptr_t> (align) - 1));

	reinterpret_cast<void **> (out)[-1] = base;
	return out;
}

void
aligned_delete (void *p, std::size_t align)
{
	if (p == nullptr)
		return;

	if (align <= allocator_align)
		g_free (p);
	else
		g_free (reinterpret_cast<void **> (p)[-1]);
}

} // namespace

void *
operator new (std::size_t size)
{
	return g_malloc (at_least_one (size));
}

void *
operator new[] (std::size_t size)
{
	return g_malloc (at_least_one (size));
}

void *
operator new (std::size_t size, const std::nothrow_t &) noexcept
{
	return g_try_malloc (at_least_one (size));
}

void *
operator new[] (std::size_t size, const std::nothrow_t &) noexcept
{
	return g_try_malloc (at_least_one (size));
}

void *
operator new (std::size_t size, std::align_val_t align)
{
	return aligned_new (size, static_cast<std::size_t> (align), alloc_or_die);
}

void *
operator new[] (std::size_t size, std::align_val_t align)
{
	return aligned_new (size, static_cast<std::size_t> (align), alloc_or_die);
}

void *
operator new (std::size_t size, std::align_val_t align, const std::nothrow_t &) noexcept
{
	return aligned_new (size, static_cast<std::size_t> (align), alloc_or_null);
}

void *
operator new[] (std::size_t size, std::align_val_t align, const std::nothrow_t &) noexcept
{
	return aligned_new (size, static_cast<std::size_t> (align), alloc_or_null);
}

void operator delete (void *p) noexcept { g_free (p); }
void operator delete[] (void *p) noexcept { g_free (p); }
void operator delete (void *p, std::size_t) noexcept { g_free (p); }
void operator delete[] (void *p, std::size_t) noexcept { g_free (p); }
void operator delete (void *p, const std::nothrow_t &) noexcept { g_free (p); }
void operator delete[] (void *p, const std::nothrow_t &) noexcept { g_free (p); }

void
operator delete (void *p, std::align_val_t align) noexcept
{
	aligned_delete (p, static_cast<std::size_t> (align));
}

void
operator delete[] (void *p, std::align_val_t align) noexcept
{
	aligned_delete (p, static_cast<std::size_t> (align));
}

void
operator delete (void *p, std::size_t, std::align_val_t align) noexcept
{
	aligned_delete (p, static_cast<std::size_t> (align));
}

void
operator delete[] (void *p, std::size_t, std::align_val_t align) noexcept
{
	aligned_delete (p, static_cast<std::size_t> (align));
}

void
operator delete (void *p, std::align_val_t align, const std::nothrow_t &) noexcept
{
	aligned_delete (p, static_cast<std::size_t> (align));
}

void
operator delete[] (void *p, std::align_val_t align, const std::nothrow_t &) noexcept
{
	aligned_delete (p, static_cast<std::size_t> (align));
}

/*
 * Make std::string allocate through the operator new above.
 *
 * <string> declares basic_string extern, so its definition otherwise comes from
 * UnityPlayer's own libstdc++, which allocates on the engine heap.  The
 * instantiation below emits a copy in this library instead.
 */
template class std::__cxx11::basic_string<char>;
