/**
 * \file
 * \brief The GC map hooks the code generator calls, which build nothing.
 *
 * mini-gc.h says what makes that right.
 */

#include "compile.h"
#include "mini-gc.h"

void
mini_gc_create_gc_map (MonoCompile *cfg)
{
}

void
mini_gc_init_cfg (MonoCompile *cfg)
{
}

void
mini_gc_set_slot_type_from_fp (MonoCompile *cfg, int slot_offset, StackSlotType type)
{
}

void
mini_gc_set_slot_type_from_cfa (MonoCompile *cfg, int slot_offset, StackSlotType type)
{
}
