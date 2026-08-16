// The native functions the P/Invoke tests call.
//
// The interpreter sorts a native call by its signature, and these cover the
// shapes it sorts into: each arity up to six over void and non-void, a struct
// returned in a register pair, a buffer written through, a callback, an errno,
// and a variadic call.  A test picks the shape it is about, so each body only
// has to be small and predictable.
//
// A test reaches these with DllImport ("__Internal"), which resolves a symbol
// in the running process rather than in a shared library.  The runtime still
// builds the marshalling wrapper, which is what the tests are about, and the
// suite names no C library.  The runner is linked with ENABLE_EXPORTS, so the
// names reach the dynamic symbol table.

#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" {

// ------------------------------------------------------------------ integers

int
interp_test_abs (int value)
{
	// Negating INT_MIN overflows, so the negation is done unsigned.
	return value < 0 ? (int) (0u - (unsigned int) value) : value;
}

int64_t
interp_test_labs (int64_t value)
{
	return value < 0 ? (int64_t) (0ull - (uint64_t) value) : value;
}

int
interp_test_toupper (int c)
{
	return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

/// The two results of a division, returned by value.  Two ints fit one
/// register, so this comes back in rax rather than through a return buffer.
struct InterpTestDiv {
	int quotient;
	int remainder;
};

InterpTestDiv
interp_test_div (int numerator, int denominator)
{
	InterpTestDiv result;
	result.quotient = numerator / denominator;
	result.remainder = numerator % denominator;
	return result;
}

// ------------------------------------------------------------------- strings

size_t
interp_test_strlen (const char *text)
{
	return strlen (text);
}

char *
interp_test_strchr (char *text, int c)
{
	return strchr (text, c);
}

int
interp_test_strncmp (const char *first, const char *second, size_t count)
{
	return strncmp (first, second, count);
}

unsigned long
interp_test_strtoul (const char *text, char **end, int base)
{
	return strtoul (text, end, base);
}

double
interp_test_atof (const char *text)
{
	return strtod (text, nullptr);
}

// A variadic callee.  On amd64 that is a calling convention of its own: the
// caller says in al how many vector registers it passed.
int
interp_test_format (char *buffer, size_t size, const char *format, ...)
{
	va_list args;
	va_start (args, format);
	int written = vsnprintf (buffer, size, format, args);
	va_end (args);
	return written;
}

// ------------------------------------------------------------------- buffers

int
interp_test_memcmp (const void *first, const void *second, size_t count)
{
	return memcmp (first, second, count);
}

void *
interp_test_memcpy (void *target, const void *source, size_t count)
{
	return memcpy (target, source, count);
}

// A caller puts the address a large value type comes back at in the first
// argument register too.  That is what makes this the callee for a returned
// struct that does not fit a register.
void *
interp_test_memset (void *target, int value, size_t count)
{
	return memset (target, value, count);
}

void
interp_test_bzero (void *target, size_t count)
{
	memset (target, 0, count);
}

void
interp_test_free (void *address)
{
	free (address);
}

void
interp_test_sort (void *items, size_t count, size_t size,
                  int (*compare) (const void *, const void *))
{
	qsort (items, count, size, compare);
}

// ------------------------------------------------------------ floating point

// Splits a double into a mantissa in [0.5, 1) and an exponent.  The exponent
// comes back through a pointer, which is an out argument for a test to
// marshal.
double
interp_test_frexp (double value, int *exponent)
{
	return frexp (value, exponent);
}

// ---------------------------------------------------------------- no result

void
interp_test_nothing (void)
{
}

static int noted;

// Nothing reads noted.  The store is there so that the argument is used.
void
interp_test_note (int value)
{
	noted = value;
}

// --------------------------------------------------------------- no argument

int
interp_test_pagesize (void)
{
	return 4096;
}

// Counts up from one.  A test can ask that the call happened without being
// able to predict what it answered.
int
interp_test_next_id (void)
{
	static int next;
	return ++next;
}

// ------------------------------------------------------------------ by name

static const struct {
	const char *name;
	void *address;
} interp_test_symbols [] = {
	{ "abs",     (void *) interp_test_abs },
	{ "labs",    (void *) interp_test_labs },
	{ "toupper", (void *) interp_test_toupper },
	{ "strlen",  (void *) interp_test_strlen },
};

/// Hands back the address of one of the functions in the table, or null.  An IL
/// test resolves a name this way and then calls what it got with `calli`.
void *
interp_test_symbol (const char *name)
{
	for (const auto &entry : interp_test_symbols)
		if (strcmp (entry.name, name) == 0)
			return entry.address;
	return nullptr;
}

/// Reports whether interp_test_symbol () knows a name, in the shape a C library
/// call reports a failure: zero, or -1 with errno set.  The mode argument is
/// never read -- it is there to make this the two-argument prototype.
int
interp_test_lookup (const char *name, int mode)
{
	(void) mode;
	if (interp_test_symbol (name))
		return 0;
	errno = ENOENT;
	return -1;
}

}
