/**
 * \file
 * \brief Reads MONO_JIT_DUMP, MONO_JIT_DUMP_DIR and MONO_JIT_DUMP_FILTER, and
 * opens the destination a dump writes to.
 */

#include "config.h"

#include "jit-dump.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <glib.h>

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/metadata-internals.h"

namespace mono {

namespace {

/// A point's name in MONO_JIT_DUMP, the directory its files land in, and the
/// extension those files take.
struct PointNames {
	DumpPoint point;
	const char *name;
	const char *extension;
};

/// The one table the variable, the directory layout and the file names are all
/// read from. A new point is a row here and a `dumping ()` call at the place it
/// describes.
const PointNames points[] = {
	{ DumpPoint::il, "il", "il" },
	{ DumpPoint::mint, "mint", "mint" },
	{ DumpPoint::unopt_ir, "unopt-ir", "ll" },
	{ DumpPoint::tier1_ir, "tier1-ir", "ll" },
	{ DumpPoint::tier2_ir, "tier2-ir", "ll" },
	{ DumpPoint::tier1_asm, "tier1-asm", "s" },
	{ DumpPoint::tier2_asm, "tier2-asm", "s" },
};

const PointNames &
names_of (DumpPoint point)
{
	for (const PointNames &row : points)
		if (row.point == point)
			return row;

	g_assert_not_reached ();
	return points[0];
}

/// Turns the list MONO_JIT_DUMP holds into the points it names.
uint32_t
parse_points (const char *setting)
{
	// A shell needs `;` quoted, so `,` and whitespace separate a list as well.
	uint32_t enabled = 0;
	const char *p = setting;

	while (*p != '\0') {
		while (*p == ';' || *p == ',' || isspace ((unsigned char) *p))
			p++;

		const char *start = p;

		while (*p != '\0' && *p != ';' && *p != ','
		       && !isspace ((unsigned char) *p))
			p++;

		size_t length = (size_t) (p - start);

		if (length == 0)
			continue;

		if (length == 3 && strncmp (start, "all", 3) == 0) {
			for (const PointNames &row : points)
				enabled |= (uint32_t) row.point;
			continue;
		}

		bool matched = false;

		for (const PointNames &row : points) {
			if (strlen (row.name) == length
			    && strncmp (start, row.name, length) == 0) {
				enabled |= (uint32_t) row.point;
				matched = true;
				break;
			}
		}

		// A name nothing matches is a typo, and a point that silently stays
		// off reads as a stage that printed nothing. Listing the names that
		// were accepted is what makes the message actionable.
		if (!matched) {
			std::string known;

			for (const PointNames &row : points)
				known += std::string (known.empty () ? "" : " ") + row.name;

			fprintf (stderr,
			         "MONO_JIT_DUMP: no dump point is called '%.*s'. "
			         "The names are: %s all\n",
			         (int) length, start, known.c_str ());
		}
	}

	return enabled;
}

uint32_t
enabled_points ()
{
	static uint32_t enabled = [] {
		const char *setting = g_getenv ("MONO_JIT_DUMP");

		return setting != nullptr ? parse_points (setting) : 0u;
	}();

	return enabled;
}

/// Serializes the dumps that share stdout. Promotions compile on several worker
/// threads, and a dump runs to many lines, so without this two methods print
/// into each other and neither is readable. A dump with a file of its own does
/// not take it.
std::mutex stdout_lock;

/// What MONO_JIT_DUMP_FILTER holds, or null when every method is dumped.
const char *
filter ()
{
	static const char *substring = [] () -> const char * {
		const char *setting = g_getenv ("MONO_JIT_DUMP_FILTER");

		return setting != nullptr && setting[0] != '\0' ? setting : nullptr;
	}();

	return substring;
}

/// The directory MONO_JIT_DUMP_DIR names, or null when dumps go to stdout.
const char *
dump_directory ()
{
	static const char *dir = [] () -> const char * {
		const char *setting = g_getenv ("MONO_JIT_DUMP_DIR");

		return setting != nullptr && setting[0] != '\0' ? setting : nullptr;
	}();

	return dir;
}

/// Turns a method's dump name into a file name.
std::string
file_stem (const char *name)
{
	// A method name holds characters a path cannot carry - `/` in a generic
	// argument, and the spaces of a signature - so each run of them becomes one
	// underscore. The cut leaves room for the extension and the uniquing
	// suffix, against the 255 bytes a file name has here.
	constexpr size_t max_stem = 200;
	std::string stem;
	bool last_was_separator = false;

	for (size_t i = 0; name[i] != '\0' && i < max_stem; i++) {
		char c = name[i];

		if (isalnum ((unsigned char) c) || c == '.' || c == '-') {
			stem.push_back (c);
			last_was_separator = false;
		} else if (!last_was_separator) {
			stem.push_back ('_');
			last_was_separator = true;
		}
	}

	return stem.empty () ? std::string ("method") : stem;
}

/// Opens the file a dump goes to, or reports on stderr and returns null.
FILE *
open_dump_file (DumpPoint point, const char *name)
{
	const PointNames &how = names_of (point);
	std::string dir = std::string (dump_directory ()) + "/" + how.name;

	if (g_mkdir_with_parents (dir.c_str (), 0755) != 0) {
		fprintf (stderr, "MONO_JIT_DUMP_DIR: cannot create %s: %s\n",
		         dir.c_str (), g_strerror (errno));
		return nullptr;
	}

	std::string stem = file_stem (name);

	// Two dumps can want one name: a generic instantiation differs from another
	// only in characters the stem drops, and a method can be compiled more than
	// once. O_EXCL is what makes counting up a suffix safe while several compile
	// threads run - the loser of a race gets EEXIST and takes the next number,
	// instead of the two writing over one file.
	for (unsigned attempt = 0; attempt < 10000; attempt++) {
		std::string path = dir + "/" + stem;

		if (attempt != 0)
			path += "." + std::to_string (attempt);
		path += ".";
		path += how.extension;

		int fd = open (path.c_str (), O_WRONLY | O_CREAT | O_EXCL, 0644);

		if (fd >= 0)
			return fdopen (fd, "w");

		if (errno != EEXIST) {
			fprintf (stderr, "MONO_JIT_DUMP_DIR: cannot write %s: %s\n",
			         path.c_str (), g_strerror (errno));
			return nullptr;
		}
	}

	fprintf (stderr, "MONO_JIT_DUMP_DIR: %s already holds 10000 files named for %s\n",
	         dir.c_str (), stem.c_str ());
	return nullptr;
}

} // namespace

bool
dump_point_enabled (DumpPoint point)
{
	return (enabled_points () & (uint32_t) point) != 0;
}

bool
any_dump_point_enabled ()
{
	return enabled_points () != 0;
}

bool
dumping (DumpPoint point, const char *name)
{
	if (!dump_point_enabled (point))
		return false;

	const char *substring = filter ();

	return substring == nullptr || strstr (name, substring) != nullptr;
}

std::string
dump_name (MonoMethod *method)
{
	char *full = mono_method_full_name (method, TRUE);
	char address[32];

	snprintf (address, sizeof (address), "@%p", (void *) method);

	std::string name = std::string (full) + address;

	g_free (full);
	return name;
}

DumpDestination::DumpDestination (DumpPoint point, const char *name)
{
	if (dump_directory () == nullptr) {
		shared_stream_ = std::unique_lock<std::mutex> (stdout_lock);
		stream_ = stdout;
		return;
	}

	stream_ = open_dump_file (point, name);
	owned_ = stream_ != nullptr;
}

DumpDestination::~DumpDestination ()
{
	if (stream_ == nullptr)
		return;

	if (owned_)
		fclose (stream_);
	else
		fflush (stream_);
}

void
dump_il (FILE *out, MonoMethod *method, MonoMethodHeader *header)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	char *class_name = mono_type_full_name (m_class_get_byval_arg (method->klass));
	char *return_type = sig != nullptr
	                  ? mono_type_full_name (sig->ret)
	                  : g_strdup ("<no signature>");
	char *arguments = sig != nullptr
	                ? mono_signature_get_desc (sig, TRUE)
	                : g_strdup ("");

	fprintf (out, ".class %s\n{\n", class_name);
	fprintf (out, "  .method %s%s %s::%s (%s) cil managed\n  {\n",
	         sig != nullptr && sig->hasthis ? "instance " : "static ",
	         return_type, class_name, method->name, arguments);

	uint32_t size;
	uint32_t max_stack;
	const uint8_t *code = mono_method_header_get_code (header, &size, &max_stack);
	uint32_t locals = header->num_locals;

	fprintf (out, "    .maxstack %u\n", max_stack);

	if (locals != 0) {
		fprintf (out, "    .locals %s(\n", header->init_locals ? "init " : "");

		for (uint32_t i = 0; i < locals; i++) {
			char *type = mono_type_full_name (header->locals[i]);

			fprintf (out, "      [%u] %s%s\n", i, type,
			         i + 1 < locals ? "," : "");
			g_free (type);
		}

		fprintf (out, "    )\n");
	}

	char *il = mono_disasm_code (nullptr, method, code, code + size);

	/* The disassembly comes back as one string of unindented lines. */
	for (const char *line = il; *line != '\0';) {
		const char *end = strchr (line, '\n');
		size_t length = end != nullptr ? (size_t) (end - line) : strlen (line);

		while (length > 0 && isspace ((unsigned char) line[length - 1]))
			length--;

		if (length != 0)
			fprintf (out, "    %.*s\n", (int) length, line);

		if (end == nullptr)
			break;
		line = end + 1;
	}

	fprintf (out, "  }\n}\n");

	g_free (il);
	g_free (arguments);
	g_free (return_type);
	g_free (class_name);
}

} // namespace mono
