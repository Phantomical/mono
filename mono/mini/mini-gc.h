/**
 * \file
 */

#ifndef __MONO_MINI_GC_H__
#define __MONO_MINI_GC_H__

#include "mini.h"

void mini_gc_init (void);

/* Whether a stack slot holds a reference, at the point it is set. */
typedef enum {
	SLOT_NOREF = 0,
	SLOT_REF = 1,
	SLOT_PIN = 2
} StackSlotType;

/* Always a no-op: mini_gc_init () above never installs a thread_mark_func. */
void mini_gc_create_gc_map (MonoCompile *cfg);
void mini_gc_init_cfg (MonoCompile *cfg);
void mini_gc_set_slot_type_from_fp (MonoCompile *cfg, int slot_offset, StackSlotType type);
void mini_gc_set_slot_type_from_cfa (MonoCompile *cfg, int slot_offset, StackSlotType type);

#endif
