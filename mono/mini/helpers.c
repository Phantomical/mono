/**
 * \file
 * Assorted routines
 *
 * (C) 2003 Ximian, Inc.
 */

#include <config.h>

#include "mini.h"
#include "mini-runtime.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <mono/metadata/opcodes.h>

#ifndef HOST_WIN32
#include <unistd.h>
#endif

#ifdef ENABLE_LLVM
#include "llvm/backend.h"
#endif

#ifndef DISABLE_JIT

#ifndef DISABLE_LOGGING

#ifdef MINI_OP
#undef MINI_OP
#endif
#ifdef MINI_OP3
#undef MINI_OP3
#endif

// This, instead of an array of pointers, to optimize away a pointer and a relocation per string.
#define MSGSTRFIELD(line) MSGSTRFIELD1(line)
#define MSGSTRFIELD1(line) str##line
static const struct msgstr_t {
#define MINI_OP(a,b,dest,src1,src2) char MSGSTRFIELD(__LINE__) [sizeof (b)];
#define MINI_OP3(a,b,dest,src1,src2,src3) char MSGSTRFIELD(__LINE__) [sizeof (b)];
#include "mini-ops.h"
#undef MINI_OP
#undef MINI_OP3
} opstr = {
#define MINI_OP(a,b,dest,src1,src2) b,
#define MINI_OP3(a,b,dest,src1,src2,src3) b,
#include "mini-ops.h"
#undef MINI_OP
#undef MINI_OP3
};
static const gint16 opidx [] = {
#define MINI_OP(a,b,dest,src1,src2)       offsetof (struct msgstr_t, MSGSTRFIELD(__LINE__)),
#define MINI_OP3(a,b,dest,src1,src2,src3) offsetof (struct msgstr_t, MSGSTRFIELD(__LINE__)),
#include "mini-ops.h"
#undef MINI_OP
#undef MINI_OP3
};

#endif /* DISABLE_LOGGING */

#if defined(__i386__) || defined(__x86_64__)
#if !defined(TARGET_ARM64) && !defined(__APPLE__)
#define emit_debug_info  TRUE
#else
#define emit_debug_info  FALSE
#endif
#else
#define emit_debug_info  FALSE
#endif

/*This enables us to use the right tooling when building the cross compiler for iOS.*/
#if defined (__APPLE__) && defined (TARGET_ARM) && (defined(__i386__) || defined(__x86_64__))

//#define ARCH_PREFIX "/Developer/Platforms/iPhoneOS.platform/Developer/usr/bin/"

#endif

#define ARCH_PREFIX ""
//#define ARCH_PREFIX "powerpc64-linux-gnu-"

const char*
mono_inst_name (int op) {
#ifndef DISABLE_LOGGING
	if (op >= OP_LOAD && op <= OP_LAST)
		return (const char*)&opstr + opidx [op - OP_LOAD];
	if (op < OP_LOAD)
		return mono_opcode_name (op);
	g_error ("unknown opcode name for %d", op);
	return NULL;
#else
	g_error ("unknown opcode name for %d", op);
	g_assert_not_reached ();
#endif
}

void
mono_blockset_print (MonoCompile *cfg, MonoBitSet *set, const char *name, guint idom) 
{
#ifndef DISABLE_LOGGING
	int i;

	if (name)
		g_print ("%s:", name);
	
	mono_bitset_foreach_bit (set, i, cfg->num_bblocks) {
		if (idom == i)
			g_print (" [BB%d]", cfg->bblocks [i]->block_num);
		else
			g_print (" BB%d", cfg->bblocks [i]->block_num);
		
	}
	g_print ("\n");
#endif
}

#ifndef DISABLE_LOGGING

/* Ascending compare of two offsets stored as GINT_TO_POINTER, for g_list_sort. */
static gint
cmp_offset_ptr (gconstpointer a, gconstpointer b)
{
	return GPOINTER_TO_INT (a) - GPOINTER_TO_INT (b);
}

/*
 * disasm_line_offset:
 *
 *   If LINE is an objdump instruction line ("   NN:\t<bytes>\t<insn>"), return
 * the leading hex offset NN. Otherwise (headers, symbol lines, source-line
 * annotations) return -1.
 */
static int
disasm_line_offset (const char *line)
{
	const char *p = line;
	char *end = NULL;
	long off;

	while (*p == ' ' || *p == '\t')
		p++;
	if (!g_ascii_isxdigit (*p))
		return -1;
	off = strtol (p, &end, 16);
	/* An instruction line is "<hex>:\t..."; the colon+tab is the tell. */
	if (end == p || end [0] != ':' || end [1] != '\t')
		return -1;
	return (int) off;
}

/*
 * disasm_branch_target:
 *
 *   If LINE carries a branch/call operand that objdump resolved to a symbol,
 * of the form "<addr> <sym+0xoff>" (or "<addr> <sym>"), set *TARGET to the
 * numeric ADDR (a full 64-bit value — external targets are huge because the
 * code buffer was relocated to 0) and [*repl_start,*repl_end) to the span of
 * "<addr> <sym...>" so the caller may rewrite it, and return TRUE. Returns
 * FALSE if the line has no such operand.
 */
static gboolean
disasm_branch_target (const char *line, int *repl_start, int *repl_end, guint64 *target)
{
	const char *lt = strrchr (line, '<');
	const char *gt;
	const char *addr_end;
	const char *addr_start;
	gchar *e = NULL;

	if (!lt)
		return FALSE;
	gt = strchr (lt, '>');
	if (!gt)
		return FALSE;
	/* A '#' before the symbol means this is a RIP-relative data reference
	 * comment ("# 0xNN <sym>"), not a branch/call target — leave it alone. */
	if (memchr (line, '#', lt - line))
		return FALSE;

	/* Back up over the single space before '<' to the address token. */
	addr_end = lt;
	while (addr_end > line && addr_end [-1] == ' ')
		addr_end--;
	addr_start = addr_end;
	while (addr_start > line && g_ascii_isxdigit (addr_start [-1]))
		addr_start--;
	if (addr_start == addr_end)
		return FALSE;
	/* The address must be a standalone whitespace-delimited token. */
	if (addr_start > line && addr_start [-1] != ' ' && addr_start [-1] != '\t')
		return FALSE;

	*target = g_ascii_strtoull (addr_start, &e, 16);
	if (e != addr_end)
		return FALSE;

	*repl_start = (int) (addr_start - line);
	*repl_end = (int) (gt + 1 - line);
	return TRUE;
}

#ifdef ENABLE_LLVM
/*
 * resolve_llvm_call_target:
 *
 *   TARGET is the runtime address a tier-1 call instruction actually
 * transfers control to. Try the JIT engine's symbol registry directly first.
 *
 *   Mono's native trampolines and icall wrappers can live anywhere in the
 * address space, unlike JITted code, which is confined to the region the
 * JIT's own memory mapper reserves. When such a target doesn't fit a rel32
 * call, JITLink's ELF x86-64 backend bridges the reach with a small stub
 * co-located with the calling code - "jmp *disp32(%rip)" through a nearby
 * GOT slot holding the real absolute address - and it's the stub's address,
 * not the callee's, that ends up baked into the call instruction. Recognize
 * that exact byte pattern and follow it to the real address before giving
 * up. The GOT-slot displacement is bounded to rule out chasing an address
 * that only coincidentally starts with the same two opcode bytes.
 */
static const char *
resolve_llvm_call_target (guint64 target)
{
	const char *name = mono_llvm_jit_resolve_symbol_name ((gpointer) (gsize) target);
	guint8 *stub;
	gint32 disp;
	guint8 *got_slot;
	guint64 real_target;

	if (name)
		return name;

	stub = (guint8 *) (gsize) target;
	if (stub [0] != 0xff || stub [1] != 0x25)
		return NULL;

	memcpy (&disp, stub + 2, sizeof (disp));
	if (disp > 0x10000 || disp < -0x10000)
		return NULL;

	got_slot = stub + 6 + disp;
	memcpy (&real_target, got_slot, sizeof (real_target));
	return mono_llvm_jit_resolve_symbol_name ((gpointer) (gsize) real_target);
}
#endif

/*
 * annotate_disassembly:
 *
 *   Read objdump's captured output from FP and re-emit it to stdout with
 * mono's own annotations layered on:
 *   - synthesized "L<n>:" labels at every local branch target (tier-agnostic:
 *     derived purely by scanning the disassembly), with branch operands
 *     rewritten to reference them;
 *   - for the classic tier-0 JIT, authoritative "; <type> <name>" comments on
 *     outgoing calls, correlated by native offset against cfg->patch_info;
 *   - for the LLVM tier-1 backend, the same "; <name>" comments on outgoing
 *     calls, resolved by reversing the printed (VMA-0) target back to a
 *     runtime address and looking it up in the JIT engine's symbol registry
 *     (every tier-1 call target is registered there at compile time).
 * Nothing is ever fabricated: unresolved sites are left as objdump rendered
 * them. cfg may be NULL (trampoline disassembly) — only the label pass runs.
 */
static void
annotate_disassembly (FILE *fp, MonoCompile *cfg, guint8 *code, int size)
{
	GPtrArray *lines = g_ptr_array_new ();
	GHashTable *instr_offsets = g_hash_table_new (NULL, NULL);      /* set of real insn offsets */
	GHashTable *bb_at = g_hash_table_new (NULL, NULL);              /* insn offset -> block_num+1 (tier-0) */
	GHashTable *off2label = g_hash_table_new (NULL, NULL);          /* target offset -> label index+1 */
	GHashTable *off2name = g_hash_table_new (NULL, NULL);           /* call-site offset -> name (tier-0) */
	GHashTable *targets = g_hash_table_new (NULL, NULL);            /* set of local target offsets */
	GList *target_list = NULL;
	char buf [4096];
	guint i;
	int pending_bb = -1;
	int label_next = 0;
	gboolean is_llvm = cfg && cfg->compile_llvm;

	/* Tier-0: build the authoritative call-site name map from patch_info.
	 * ji->ip.i is the native offset of the call/branch instruction. Skip
	 * MONO_PATCH_INFO_BB: those are the local branches the label pass handles. */
	if (cfg && !is_llvm) {
		MonoJumpInfo *ji;
		for (ji = cfg->patch_info; ji; ji = ji->next) {
			if (ji->type == MONO_PATCH_INFO_BB)
				continue;
			if (ji->ip.i < 0 || ji->ip.i >= size)
				continue;
			if (g_hash_table_lookup (off2name, GINT_TO_POINTER (ji->ip.i)))
				continue;
			g_hash_table_insert (off2name, GINT_TO_POINTER (ji->ip.i), mono_ji_to_string (ji));
		}
	}

	/* Pass 1: read all lines, record instruction offsets, <BB> markers, and
	 * the set of in-range local branch targets. */
	while (fgets (buf, sizeof (buf), fp)) {
		char *line = g_strdup (buf);
		size_t len = strlen (line);
		int off, rs, re;
		guint64 tgt;

		if (len && line [len - 1] == '\n')
			line [len - 1] = '\0';
		g_ptr_array_add (lines, line);

		/* objdump -l stabs marker for a block start: "<BB>:N" */
		if (g_str_has_prefix (line, "<BB>:")) {
			pending_bb = atoi (line + 5);
			continue;
		}

		off = disasm_line_offset (line);
		if (off < 0)
			continue;
		g_hash_table_insert (instr_offsets, GINT_TO_POINTER (off), GINT_TO_POINTER (1));
		if (pending_bb >= 0) {
			g_hash_table_insert (bb_at, GINT_TO_POINTER (off), GINT_TO_POINTER (pending_bb + 1));
			/* A basic-block start is a label anchor even with no incoming
			 * intra-method branch (tier-0 only; tier-1 has no bb offsets). */
			g_hash_table_insert (targets, GINT_TO_POINTER (off), GINT_TO_POINTER (1));
			pending_bb = -1;
		}
		if (disasm_branch_target (line, &rs, &re, &tgt) && tgt < (guint64) size)
			g_hash_table_insert (targets, GINT_TO_POINTER ((int) tgt), GINT_TO_POINTER (1));
	}

	/* Assign labels L0,L1,... to every label anchor (local branch target or
	 * basic-block start) that lands on a real instruction, in ascending offset
	 * order. */
	{
		GHashTableIter it;
		gpointer k;
		g_hash_table_iter_init (&it, targets);
		while (g_hash_table_iter_next (&it, &k, NULL)) {
			if (g_hash_table_lookup (instr_offsets, k))
				target_list = g_list_prepend (target_list, k);
		}
		target_list = g_list_sort (target_list, cmp_offset_ptr);
	}
	for (GList *l = target_list; l; l = l->next)
		g_hash_table_insert (off2label, l->data, GINT_TO_POINTER (++label_next));

	/* Pass 2: re-emit with labels, rewritten operands, and call names. */
	for (i = 0; i < lines->len; ++i) {
		char *line = (char *) g_ptr_array_index (lines, i);
		const char *name;
		int off, rs, re;
		guint64 tgt;
		gboolean has_target;

		/* Drop the raw <BB>:N marker; its block number is folded into the label. */
		if (g_str_has_prefix (line, "<BB>:"))
			continue;

		off = disasm_line_offset (line);
		if (off < 0) {
			printf ("%s\n", line);
			continue;
		}

		/* Emit a label line if this offset is a label anchor. */
		{
			gpointer lp = g_hash_table_lookup (off2label, GINT_TO_POINTER (off));
			if (lp) {
				gpointer bp = g_hash_table_lookup (bb_at, GINT_TO_POINTER (off));
				if (bp)
					printf ("L%d:\t\t\t\t; BB%d\n", GPOINTER_TO_INT (lp) - 1, GPOINTER_TO_INT (bp) - 1);
				else
					printf ("L%d:\n", GPOINTER_TO_INT (lp) - 1);
			}
		}

		name = (const char *) g_hash_table_lookup (off2name, GINT_TO_POINTER (off));
		has_target = disasm_branch_target (line, &rs, &re, &tgt);

		if (has_target && tgt < (guint64) size) {
			gpointer lp = g_hash_table_lookup (off2label, GINT_TO_POINTER ((int) tgt));
			if (lp) {
				/* Local branch: operand -> L<n>, keep the raw offset as a comment. */
				printf ("%.*sL%d\t\t; 0x%x\n", rs, line, GPOINTER_TO_INT (lp) - 1, (int) tgt);
				continue;
			}
			/* target in range but not a known label: leave as-is */
			printf ("%s\n", line);
			continue;
		}
		if (has_target) {
			/* External call/branch. objdump's "<fn+0xhuge>" is bogus (code was
			 * relocated to 0); replace it with the authoritative name if we have
			 * one, else leave objdump's text untouched. */
#ifdef ENABLE_LLVM
			/*
			 * Tier-1: TGT is objdump's printed target, computed as if the
			 * method were loaded at VMA 0. For a rel32 call baked in at JIT
			 * time against the method's real load address CODE, that printed
			 * value and the real runtime target differ by exactly CODE (the
			 * two ends of the relocation cancel out — see the design notes
			 * for the derivation). Recover the real target and look it up in
			 * the engine's symbol registry.
			 */
			if (!name && is_llvm && code)
				name = resolve_llvm_call_target (tgt + (guint64) (gsize) code);
#endif
			if (name)
				printf ("%.*s<target>\t; %s\n", rs, line, name);
			else
				printf ("%s\n", line);
			continue;
		}

		/* No symbolized operand. Tier-0 indirect call sites may still carry a
		 * name in patch_info (e.g. "call *%rax"): append it. */
		if (name)
			printf ("%s\t; %s\n", line, name);
		else
			printf ("%s\n", line);
	}

	{
		GHashTableIter it;
		gpointer v;
		g_hash_table_iter_init (&it, off2name);
		while (g_hash_table_iter_next (&it, NULL, &v))
			g_free (v);
	}
	g_hash_table_destroy (off2name);
	g_hash_table_destroy (off2label);
	g_hash_table_destroy (targets);
	g_hash_table_destroy (bb_at);
	g_hash_table_destroy (instr_offsets);
	g_list_free (target_list);
	for (i = 0; i < lines->len; ++i)
		g_free (g_ptr_array_index (lines, i));
	g_ptr_array_free (lines, TRUE);
}

#endif /* DISABLE_LOGGING */

/**
 * \param cfg compilation context
 * \param code a pointer to the code
 * \param size the code size in bytes
 *
 * Disassemble to code to stdout.
 */
void
mono_disassemble_code (MonoCompile *cfg, guint8 *code, int size, char *id)
{
#ifndef DISABLE_LOGGING
	GHashTable *offset_to_bb_hash = NULL;
	int i, cindex, bb_num;
	FILE *ofd;
#ifdef HOST_WIN32
	const char *tmp = g_get_tmp_dir ();
#endif
	char *as_file;
	char *o_file;
	int unused G_GNUC_UNUSED;

#ifdef HOST_WIN32
	as_file = g_strdup_printf ("%s/test.s", tmp);    

	if (!(ofd = fopen (as_file, "w")))
		g_assert_not_reached ();
#else	
	i = g_file_open_tmp (NULL, &as_file, NULL);
	ofd = fdopen (i, "w");
	g_assert (ofd);
#endif

	for (i = 0; id [i]; ++i) {
		if (i == 0 && isdigit (id [i]))
			fprintf (ofd, "_");
		else if (!isalnum (id [i]))
			fprintf (ofd, "_");
		else
			fprintf (ofd, "%c", id [i]);
	}
	fprintf (ofd, ":\n");

	if (emit_debug_info && cfg != NULL) {
		MonoBasicBlock *bb;

		fprintf (ofd, ".stabs	\"\",100,0,0,.Ltext0\n");
		fprintf (ofd, ".stabs	\"<BB>\",100,0,0,.Ltext0\n");
		fprintf (ofd, ".Ltext0:\n");

		offset_to_bb_hash = g_hash_table_new (NULL, NULL);
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
			g_hash_table_insert (offset_to_bb_hash, GINT_TO_POINTER (bb->native_offset), GINT_TO_POINTER (bb->block_num + 1));
		}
	}

	cindex = 0;
	for (i = 0; i < size; ++i) {
		if (emit_debug_info && cfg != NULL) {
			bb_num = GPOINTER_TO_INT (g_hash_table_lookup (offset_to_bb_hash, GINT_TO_POINTER (i)));
			if (bb_num) {
				fprintf (ofd, "\n.stabd 68,0,%d\n", bb_num - 1);
				cindex = 0;
			}
		}
		if (cindex == 0) {
			fprintf (ofd, "\n.byte %u", (unsigned int) code [i]);
		} else {
			fprintf (ofd, ",%u", (unsigned int) code [i]);
		}
		cindex++;
		if (cindex == 64)
			cindex = 0;
	}
	fprintf (ofd, "\n");
	fclose (ofd);

#ifdef __APPLE__
#ifdef __ppc64__
#define DIS_CMD "otool64 -v -t"
#else
#define DIS_CMD "otool -v -t"
#endif
#else
#if defined(sparc) && !defined(__GNUC__)
#define DIS_CMD "dis"
#elif defined(TARGET_X86)
#define DIS_CMD "objdump -l -d --disassemble-zeroes"
#elif defined(TARGET_AMD64)
  #if defined(HOST_WIN32)
  #define DIS_CMD "x86_64-w64-mingw32-objdump.exe -M x86-64 -d --disassemble-zeroes"
  #else
  #define DIS_CMD "objdump -l -d --disassemble-zeroes"
  #endif
#else
#define DIS_CMD "objdump -d --disassemble-zeroes"
#endif
#endif

#if defined(sparc)
#define AS_CMD "as -xarch=v9"
#elif defined (TARGET_X86)
#  if defined(__APPLE__)
#    define AS_CMD "as -arch i386"
#  else
#    define AS_CMD "as -gstabs"
#  endif
#elif defined (TARGET_AMD64)
#  if defined (__APPLE__)
#    define AS_CMD "as -arch x86_64"
#  else
#    define AS_CMD "as -gstabs"
#  endif
#elif defined (TARGET_ARM)
#  if defined (__APPLE__)
#    define AS_CMD "as -arch arm"
#  else
#    define AS_CMD "as -gstabs"
#  endif
#elif defined (TARGET_ARM64)
#  if defined (__APPLE__)
#    define AS_CMD "clang -c -arch arm64 -g -x assembler"
#  else
#    define AS_CMD "as -gstabs"
#  endif
#elif defined(__mips__) && (_MIPS_SIM == _ABIO32)
#define AS_CMD "as -mips32"
#elif defined(__ppc64__)
#define AS_CMD "as -arch ppc64"
#elif defined(__powerpc64__)
#define AS_CMD "as -mppc64"
#elif defined (TARGET_RISCV64)
#define AS_CMD "as -march=rv64ima"
#elif defined (TARGET_RISCV32)
#define AS_CMD "as -march=rv32ima"
#else
#define AS_CMD "as"
#endif

#ifdef HOST_WIN32
	o_file = g_strdup_printf ("%s/test.o", tmp);
#else	
	i = g_file_open_tmp (NULL, &o_file, NULL);
	close (i);
#endif

#ifdef HAVE_SYSTEM
	char *cmd = g_strdup_printf (ARCH_PREFIX AS_CMD " %s -o %s", as_file, o_file);
	unused = system (cmd); 
	g_free (cmd);
	char *objdump_args = g_getenv ("MONO_OBJDUMP_ARGS");
	if (!objdump_args)
		objdump_args = g_strdup ("");

	fflush (stdout);

#if (defined(__arm__) || defined(__aarch64__)) && !defined(TARGET_OSX)
	/* 
	 * The arm assembler inserts ELF directives instructing objdump to display 
	 * everything as data.
	 */
	cmd = g_strdup_printf (ARCH_PREFIX "strip -s %s", o_file);
	unused = system (cmd);
	g_free (cmd);
#endif

	cmd = g_strdup_printf (ARCH_PREFIX DIS_CMD " %s %s", objdump_args, o_file);
	/*
	 * Capture objdump's output so we can layer mono's own annotations onto it
	 * (basic-block labels + authoritative call-target names). If popen is
	 * unavailable or fails, fall back to streaming it straight to stdout, which
	 * is the historical behaviour.
	 */
	{
		FILE *dis = popen (cmd, "r");
		if (dis) {
			annotate_disassembly (dis, cfg, code, size);
			pclose (dis);
		} else {
			unused = system (cmd);
		}
	}
	g_free (cmd);
	g_free (objdump_args);
#else
	g_assert_not_reached ();
#endif /* HAVE_SYSTEM */

#ifndef HOST_WIN32
	unlink (o_file);
	unlink (as_file);
#endif
	g_free (o_file);
	g_free (as_file);
#endif
}

#else /* DISABLE_JIT */

void
mono_blockset_print (MonoCompile *cfg, MonoBitSet *set, const char *name, guint idom)
{
}

#endif /* DISABLE_JIT */
