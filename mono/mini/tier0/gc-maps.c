/**
 * \file
 * \brief The GC map hooks the code generator calls, which build no map.
 *
 * mini-gc.h says what makes that right.
 */

#include "compile.h"
#include "mini-gc.h"
#include "mono/metadata/gc-internals.h"

void
mini_gc_create_gc_map (MonoCompile *cfg)
{
}

/*
 * No map, but the collector's write barriers are decided here too, and a body
 * that skips them leaves an old-to-young reference no card records. The next
 * nursery collection then leaves that field pointing at freed memory.
 * MONO_GC_DEBUG=check-remset-consistency is what catches the miss at the
 * store rather than at the collection after it.
 */
void
mini_gc_init_cfg (MonoCompile *cfg)
{
	if (mono_gc_needs_write_barriers ()) {
		cfg->disable_ref_noref_stack_slot_share = TRUE;
		cfg->gen_write_barriers = TRUE;
	}
}

void
mini_gc_set_slot_type_from_fp (MonoCompile *cfg, int slot_offset, StackSlotType type)
{
}

void
mini_gc_set_slot_type_from_cfa (MonoCompile *cfg, int slot_offset, StackSlotType type)
{
}
