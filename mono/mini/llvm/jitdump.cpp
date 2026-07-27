/**
 * \file
 * jitdump.cpp - perf jitdump records for the bodies the LLVM tier compiles.
 *
 * perf learns about JIT'd code from /tmp/jit-<pid>.dump, a stream of records
 * that `perf inject --jit` turns into one synthetic ELF per compiled function
 * so that `perf report` can symbolize and unwind through them. mini-runtime.c
 * owns that file - perf only ever looks for the single one named after our pid,
 * so there can only be one writer - and emits a bare JIT_CODE_LOAD (name,
 * address, code bytes) for classic-JIT methods.
 *
 * A tier-1 body can say more than that, and this is where it does: LLVM emits a
 * stock DWARF .eh_frame for every method it compiles, which is exactly the
 * material a JIT_CODE_UNWINDING_INFO record wants, so `perf record --call-graph
 * dwarf` can walk out of a tier-1 frame instead of stopping at it. The classic
 * JIT has only mono's own unwind ops, which would have to be transcoded first,
 * so it keeps the plain record.
 *
 * Line numbers (JIT_CODE_DEBUG_INFO) come from the same place: the translator
 * records IL offsets as debug info and the engine reads a native_offset ->
 * il_offset map back out of the emitted object, which is exactly the line table
 * this record wants. Where the method's assembly ships symbols the rows are
 * resolved the rest of the way to real file and line; where it does not, the row
 * reports the IL offset against the method's own name, which is still enough to
 * tell two parts of one body apart in a profile.
 *
 * WHY THE .eh_frame IS REWRITTEN AND NOT COPIED
 *
 * The unwind tables perf receives do not describe our address space - they
 * describe the ELF it is about to synthesize, which lays the sections out like
 * this (tools/perf/util/genelf.c), with .text at a fixed 128-byte offset and
 * .eh_frame 8-byte aligned right behind it:
 *
 *     +-------------+ <- text        = 128
 *     |   .text     |                     (code_size bytes)
 *     +-------------+ <- eh_frame    = text + align8 (code_size)
 *     | CIE FDE 0 0 |
 *     +-------------+ <- eh_frame_hdr
 *     |  hdr + LUT  |
 *     +-------------+
 *
 * Our FDE's initial_location is pc-relative, and both the .eh_frame_hdr's
 * search table and its pointer to .eh_frame are relative to positions in that
 * layout, so every one of those offsets has to be computed for the DSO rather
 * than measured here. That is what build_perf_unwind_data () does, and it is why
 * the blob is rebuilt from just the CIE and the one FDE that covers the method
 * instead of the section being handed over whole.
 *
 * Note the section ORDER above: .eh_frame first, .eh_frame_hdr last. The
 * jitdump specification says the opposite (hdr at the start of the record's
 * payload), but perf's own reader splits the payload the other way round, and
 * perf is the only consumer there is. V8's jitdump writer made the same call,
 * against the same comment about perf's DSO layout.
 */

#include "config.h"

#include <glib.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <mono/metadata/mono-debug.h>
#include <mono/utils/mono-time.h>

#include "mini.h"
#include "mini-runtime.h"
#include "mini-unwind.h"
#include "backend.h"
#include "jitdump.hpp"

/*
 * Completing the DW_EH_PE_* set mini-unwind.h declares, the same way
 * ehframe.cpp does: these are encodings a CIE we did not write may legally use,
 * and we have to know their operand widths to skip past them.
 */
#ifndef DW_EH_PE_uleb128
#define DW_EH_PE_uleb128 0x01
#endif
#ifndef DW_EH_PE_udata2
#define DW_EH_PE_udata2 0x02
#endif
#ifndef DW_EH_PE_udata8
#define DW_EH_PE_udata8 0x04
#endif
#ifndef DW_EH_PE_sleb128
#define DW_EH_PE_sleb128 0x09
#endif
#ifndef DW_EH_PE_sdata2
#define DW_EH_PE_sdata2 0x0a
#endif

namespace {

enum {
	JIT_CODE_DEBUG_INFO = 2,
	JIT_CODE_UNWINDING_INFO = 4,
};

/*
 * The fixed head of a jitdump record. mini-runtime.c has its own copy for the
 * code-load record it writes; this is the one the unwinding record needs.
 */
struct RecordHeader {
	std::uint32_t id;
	std::uint32_t total_size;
	std::uint64_t timestamp;
};

struct DebugInfoRecord {
	RecordHeader header;
	std::uint64_t code_addr;
	std::uint64_t nr_entry;
	/* Followed by nr_entry variable-length entries: addr, lineno, discrim, name. */
};

struct UnwindingRecord {
	RecordHeader header;
	std::uint64_t unwind_data_size;
	std::uint64_t eh_frame_hdr_size;
	std::uint64_t mapped_size;
	/* Followed by unwind_data_size bytes: .eh_frame, then .eh_frame_hdr. */
};

/* perf sizes the payload as total_size minus the fixed part, so a padded
 * struct here would silently shift every byte of unwind data. */
static_assert (sizeof (RecordHeader) == 16, "jitdump record header must be 16 bytes");
static_assert (sizeof (UnwindingRecord) == 40, "jitdump unwinding record must be 40 bytes");
static_assert (sizeof (DebugInfoRecord) == 32, "jitdump debug-info record must be 32 bytes");

/*
 * A bounds-checked cursor over a CIE. Once a read runs off the end the cursor
 * fails permanently and every later read is a no-op returning 0, so a decoder
 * can walk a whole structure and test ok () once at the end.
 */
class Cursor {
public:
	Cursor (const std::uint8_t *start, const std::uint8_t *end) : p_ (start), end_ (end) {}

	bool ok () const { return ok_; }
	const std::uint8_t *pos () const { return p_; }

	std::uint8_t u8 ()
	{
		if (!take (1))
			return 0;
		return p_ [-1];
	}

	std::uint32_t u32 ()
	{
		if (!take (4))
			return 0;
		std::uint32_t v;
		std::memcpy (&v, p_ - 4, 4);
		return v;
	}

	void skip (std::size_t n) { take (n); }

	/*
	 * Nothing here needs a LEB's value, only its length - and signed and
	 * unsigned LEBs are framed identically, so one skip covers both.
	 */
	void skip_leb ()
	{
		while (take (1)) {
			if (!(p_ [-1] & 0x80))
				return;
		}
	}

	bool skip_cstr ()
	{
		while (take (1)) {
			if (p_ [-1] == 0)
				return true;
		}
		return false;
	}

private:
	bool take (std::size_t n)
	{
		if (!ok_ || static_cast<std::size_t> (end_ - p_) < n) {
			ok_ = false;
			return false;
		}
		p_ += n;
		return true;
	}

	const std::uint8_t *p_;
	const std::uint8_t *end_;
	bool ok_ = true;
};

/* Step over a pointer encoded as ENCODING. FALSE if the encoding is one we
 * cannot size, which makes the rest of the CIE unreadable. */
bool
skip_encoded_pointer (Cursor &c, std::uint8_t encoding)
{
	if (encoding == DW_EH_PE_omit)
		return true;

	switch (encoding & 0x0f) {
	case DW_EH_PE_absptr:
	case DW_EH_PE_udata8:
	case DW_EH_PE_sdata8:
		c.skip (8);
		break;
	case DW_EH_PE_uleb128:
	case DW_EH_PE_sleb128:
		c.skip_leb ();
		break;
	case DW_EH_PE_udata2:
	case DW_EH_PE_sdata2:
		c.skip (2);
		break;
	case DW_EH_PE_udata4:
	case DW_EH_PE_sdata4:
		c.skip (4);
		break;
	default:
		return false;
	}

	return c.ok ();
}

/*
 * The pointer encoding CIE declares for its FDEs' initial_location, or -1 if it
 * cannot be read. A CIE with no 'R' augmentation declares nothing, which means
 * plain absolute addresses.
 */
int
cie_fde_encoding (const std::uint8_t *cie, const std::uint8_t *cie_end)
{
	Cursor c (cie, cie_end);

	c.skip (4);
	if (c.u32 () != 0)
		return -1;

	std::uint8_t version = c.u8 ();
	if (version != 1 && version != 3 && version != 4)
		return -1;

	const char *aug = reinterpret_cast<const char *> (c.pos ());
	if (!c.skip_cstr ())
		return -1;

	if (version == 4)
		c.skip (2); /* address_size, segment_selector_size */
	c.skip_leb ();      /* code alignment factor */
	c.skip_leb ();      /* data alignment factor */
	if (version == 1)
		c.skip (1); /* return address register */
	else
		c.skip_leb ();

	if (aug [0] != 'z')
		return c.ok () ? DW_EH_PE_absptr : -1;

	c.skip_leb ();      /* augmentation data length */

	int encoding = DW_EH_PE_absptr;
	for (const char *a = aug + 1; *a; a++) {
		switch (*a) {
		case 'R':
			encoding = c.u8 ();
			break;
		case 'L':
			c.skip (1);
			break;
		case 'P':
			if (!skip_encoded_pointer (c, c.u8 ()))
				return -1;
			break;
		case 'S':
		case 'B':
		case 'G':
			break; /* flags, no operand */
		default:
			/* An augmentation we cannot size means everything after it
			 * is at an unknown offset. */
			return -1;
		}
	}

	return c.ok () ? encoding : -1;
}

/* One method's CIE and FDE, as found in the section LLVM emitted. */
struct FdeLocation {
	const std::uint8_t *cie = nullptr;
	std::uint32_t cie_size = 0;
	const std::uint8_t *fde = nullptr;
	std::uint32_t fde_size = 0;
};

/*
 * Find the FDE describing the function at CODE.
 *
 * The match is on the FDE's own initial_location rather than on position,
 * because a method's object can hold more than one function - the GC safepoint
 * poll rides along with it.
 */
bool
find_fde (const std::uint8_t *section, std::uint32_t size, const std::uint8_t *code, FdeLocation &out)
{
	const std::uint8_t *end = section + size;
	const std::uint8_t *p = section;

	while (end - p >= 4) {
		std::uint32_t length;
		std::memcpy (&length, p, 4);

		if (length == 0)
			break; /* the zero word that terminates the section */
		/* 64-bit DWARF, which LLVM does not emit here. Decline rather
		 * than misread the following words. */
		if (length == 0xffffffff)
			return false;
		if (length > static_cast<std::uint32_t> (end - p - 4))
			return false;
		/* Every entry starts with the id word, and an FDE follows it with
		 * initial_location. Checked before either is read, so a record
		 * claiming a short length cannot walk us off the end. */
		if (length < 4)
			return false;

		const std::uint8_t *entry = p;
		const std::uint8_t *entry_end = p + 4 + length;
		std::uint32_t id;

		std::memcpy (&id, p + 4, 4);
		p = entry_end;

		if (id == 0)
			continue; /* a CIE; FDEs point at their own */

		if (length < 8)
			return false;

		/* In .eh_frame the id is the distance back from its own position
		 * to the CIE, not a section offset. */
		if (id > static_cast<std::uint32_t> ((entry + 4) - section))
			return false;

		const std::uint8_t *cie = entry + 4 - id;
		std::uint32_t cie_length;

		if (end - cie < 4)
			return false;
		std::memcpy (&cie_length, cie, 4);
		if (cie_length == 0 || cie_length == 0xffffffff ||
		    cie_length > static_cast<std::uint32_t> (end - cie - 4))
			return false;

		/*
		 * Only the encoding LLVM emits under the small PIC code model is
		 * handled. Relocating any other one for the injected DSO is work
		 * we have no way to test, and saying nothing is always safe.
		 */
		if (cie_fde_encoding (cie, cie + 4 + cie_length) != (DW_EH_PE_pcrel | DW_EH_PE_sdata4))
			return false;

		std::int32_t rel;
		std::memcpy (&rel, entry + 8, 4);
		if (entry + 8 + rel != code)
			continue;

		out.cie = cie;
		out.cie_size = 4 + cie_length;
		out.fde = entry;
		out.fde_size = 4 + length;
		return true;
	}

	return false;
}

void
write32 (std::vector<std::uint8_t> &buf, std::uint32_t offset, std::int32_t value)
{
	std::memcpy (buf.data () + offset, &value, 4);
}

} /* anonymous namespace */

bool
mono::build_perf_debug_info (std::uint64_t code_addr,
                             const std::vector<mono::PerfDebugEntry> &entries,
                             std::vector<std::uint8_t> &out)
{
	if (entries.empty ())
		return false;

	/*
	 * Entries are variable length - a fixed head and then a NUL-terminated name -
	 * and perf reads them by walking, not by indexing, so nothing here is padded
	 * or aligned.
	 */
	std::size_t payload = 0;
	for (const mono::PerfDebugEntry &e : entries)
		payload += 8 + 4 + 4 + e.name.size () + 1;

	DebugInfoRecord rec;
	std::memset (&rec, 0, sizeof (rec));
	rec.header.id = JIT_CODE_DEBUG_INFO;
	rec.header.total_size = (std::uint32_t) (sizeof (rec) + payload);
	rec.header.timestamp = mono_clock_get_time_ns (CLOCK_MONOTONIC);
	rec.code_addr = code_addr;
	rec.nr_entry = entries.size ();

	out.resize (sizeof (rec) + payload);
	std::memcpy (out.data (), &rec, sizeof (rec));

	std::size_t at = sizeof (rec);
	for (const mono::PerfDebugEntry &e : entries) {
		std::memcpy (out.data () + at, &e.addr, 8);
		at += 8;
		std::memcpy (out.data () + at, &e.lineno, 4);
		at += 4;
		std::memcpy (out.data () + at, &e.discrim, 4);
		at += 4;
		std::memcpy (out.data () + at, e.name.c_str (), e.name.size () + 1);
		at += e.name.size () + 1;
	}

	return true;
}

bool
mono::build_perf_unwind_data (const std::uint8_t *code, std::uint32_t code_size,
                              const std::uint8_t *eh_frame, std::uint32_t eh_frame_size,
                              std::vector<std::uint8_t> &out)
{
	FdeLocation fde;

	if (!code || !code_size || !eh_frame || eh_frame_size < 4)
		return false;
	if (!find_fde (eh_frame, eh_frame_size, code, fde))
		return false;

	const std::uint32_t fde_off = fde.cie_size;
	/* The CIE, the FDE, and the zero word that ends a .eh_frame. */
	const std::uint32_t frame_size = fde.cie_size + fde.fde_size + 4;
	/* .text to .eh_frame in the injected DSO. Both start 8-byte aligned, so
	 * the gap is just the padding behind the code. */
	const std::uint32_t code_to_frame = ALIGN_TO (code_size, PERF_ELF_EH_FRAME_ALIGN);

	out.assign (frame_size + PERF_EH_FRAME_HDR_SIZE, 0);
	std::memcpy (out.data (), fde.cie, fde.cie_size);
	std::memcpy (out.data () + fde_off, fde.fde, fde.fde_size);

	/* Both of the FDE's back-references move with it: the CIE pointer counts
	 * back from its own position to a CIE now at offset 0, ... */
	write32 (out, fde_off + 4, fde_off + 4);
	/* ... and initial_location has to span the DSO's distance from the FDE
	 * to the code, not the one our own mapping happens to have. */
	write32 (out, fde_off + 8, -static_cast<std::int32_t> (code_to_frame + fde_off + 8));

	const std::uint32_t hdr = frame_size;

	out [hdr + 0] = 1;                                  /* version */
	out [hdr + 1] = DW_EH_PE_pcrel | DW_EH_PE_sdata4;   /* eh_frame_ptr */
	out [hdr + 2] = DW_EH_PE_udata4;                    /* fde_count */
	out [hdr + 3] = DW_EH_PE_datarel | DW_EH_PE_sdata4; /* table entries */
	/* Back over the header bytes read so far and over the whole .eh_frame. */
	write32 (out, hdr + 4, -static_cast<std::int32_t> (frame_size + 4));
	write32 (out, hdr + 8, 1);
	/* Table entries are relative to the start of the header. */
	write32 (out, hdr + 12, -static_cast<std::int32_t> (code_to_frame + frame_size));
	write32 (out, hdr + 16, -static_cast<std::int32_t> (frame_size - fde_off));

	return true;
}

/*
 * Turn this body's native_offset -> il_offset map into the rows perf wants.
 *
 * Where the method's assembly ships symbols each IL offset resolves the rest of
 * the way to a real source file and line. Where it does not - the common case
 * for a released assembly - the row still carries the IL offset, reported
 * against the method's own name, which is what makes two parts of one body
 * distinguishable in a profile at all.
 */
static void
collect_debug_entries (MonoMethod *method, gpointer code, const char *name,
                       const MonoLLVMSeqPoint *seq_points, guint32 n_seq_points,
                       std::vector<mono::PerfDebugEntry> &out)
{
	MonoDomain *domain = mono_domain_get ();

	out.reserve (n_seq_points);

	for (guint32 i = 0; i < n_seq_points; ++i) {
		mono::PerfDebugEntry entry;

		entry.addr = (std::uint64_t) (gsize) code + seq_points [i].native_offset;
		entry.discrim = 0;

		MonoDebugSourceLocation *loc =
			mono_debug_lookup_source_location_by_il (method, seq_points [i].il_offset, domain);
		if (loc && loc->source_file) {
			entry.lineno = (std::int32_t) loc->row;
			entry.name = loc->source_file;
		} else {
			/* perf numbers lines from 1, and an IL offset starts at 0. */
			entry.lineno = (std::int32_t) (seq_points [i].il_offset + 1);
			entry.name = name;
		}
		if (loc)
			mono_debug_free_source_location (loc);

		out.push_back (std::move (entry));
	}
}

void
mono_llvm_jitdump_emit_method (MonoMethod *method, gpointer code, guint32 code_size,
                               const guint8 *eh_frame, guint32 eh_frame_size,
                               const MonoLLVMSeqPoint *seq_points, guint32 n_seq_points)
{
	std::vector<std::uint8_t> unwind_data;
	std::vector<std::uint8_t> debug_record;
	std::vector<std::uint8_t> record;
	char *name;

	if (!mono_jit_dump_is_enabled ())
		return;

	/*
	 * The full name, so that the two bodies a promoted method leaves behind
	 * are told apart by more than their addresses - the tier-0 record carries
	 * the bare method name.
	 */
	name = mono_method_full_name (method, TRUE);

	if (seq_points && n_seq_points) {
		std::vector<mono::PerfDebugEntry> entries;

		collect_debug_entries (method, code, name, seq_points, n_seq_points, entries);
		mono::build_perf_debug_info ((std::uint64_t) (gsize) code, entries, debug_record);
	}

	if (mono::build_perf_unwind_data (static_cast<const std::uint8_t *> (code), code_size,
	                                  eh_frame, eh_frame_size, unwind_data)) {
		UnwindingRecord uwr;

		memset (&uwr, 0, sizeof (uwr));
		uwr.header.id = JIT_CODE_UNWINDING_INFO;
		uwr.header.total_size = (std::uint32_t) (sizeof (uwr) + unwind_data.size ());
		uwr.header.timestamp = mono_clock_get_time_ns (CLOCK_MONOTONIC);
		uwr.unwind_data_size = unwind_data.size ();
		uwr.eh_frame_hdr_size = mono::PERF_EH_FRAME_HDR_SIZE;
		/* Zero because what we hand over is a rewritten copy, not a region
		 * the process has mapped anywhere. */
		uwr.mapped_size = 0;

		record.resize (sizeof (uwr) + unwind_data.size ());
		std::memcpy (record.data (), &uwr, sizeof (uwr));
		std::memcpy (record.data () + sizeof (uwr), unwind_data.data (), unwind_data.size ());
	}

	/*
	 * Both records attach to the next code load perf reads, so they go over as
	 * one blob written under the file's lock rather than as separate calls.
	 */
	if (!debug_record.empty ())
		record.insert (record.begin (), debug_record.begin (), debug_record.end ());

	mono_emit_jit_dump_code (name, code, code_size,
	                         record.empty () ? NULL : record.data (),
	                         (guint32) record.size ());
	g_free (name);
}
