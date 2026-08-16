#ifndef __MONO_INTERP_FRAME_DATA_HPP__
#define __MONO_INTERP_FRAME_DATA_HPP__

#include "interp-internals.h"

namespace mono::interp {

/*
 * This code synchronizes with interp_mark_stack () using compiler memory barriers.
 */

inline FrameDataFragment *
frame_data_frag_new (int size)
{
	FrameDataFragment *frag = (FrameDataFragment *) g_malloc (size);

	frag->pos = (guint8 *) &frag->data;
	frag->end = (guint8 *) frag + size;
	frag->next = NULL;
	return frag;
}

inline void
frame_data_frag_free (FrameDataFragment *frag)
{
	while (frag) {
		FrameDataFragment *next = frag->next;
		g_free (frag);
		frag = next;
	}
}

inline void
frame_data_allocator_init (FrameDataAllocator *stack, int size)
{
	FrameDataFragment *frag;

	frag = frame_data_frag_new (size);
	stack->first = stack->current = frag;
	stack->infos_capacity = 4;
	stack->infos = (FrameDataInfo *) g_malloc (stack->infos_capacity * sizeof (FrameDataInfo));
}

inline void
frame_data_allocator_free (FrameDataAllocator *stack)
{
	/* Assert to catch leaks */
	g_assert_checked (stack->current == stack->first
	                  && stack->current->pos == (guint8 *) &stack->current->data);
	frame_data_frag_free (stack->first);
}

inline FrameDataFragment *
frame_data_allocator_add_frag (FrameDataAllocator *stack, int size)
{
	FrameDataFragment *new_frag;

	// FIXME:
	int frag_size = 4096;
	if (size + sizeof (FrameDataFragment) > frag_size)
		frag_size = size + sizeof (FrameDataFragment);
	new_frag = frame_data_frag_new (frag_size);
	mono_compiler_barrier ();
	stack->current->next = new_frag;
	stack->current = new_frag;
	return new_frag;
}

inline gpointer
frame_data_allocator_alloc (FrameDataAllocator *stack, InterpFrame *frame, int size)
{
	FrameDataFragment *current = stack->current;
	gpointer res;

	int infos_len = stack->infos_len;

	if (!infos_len || (infos_len > 0 && stack->infos[infos_len - 1].frame != frame)) {
		/* First allocation by this frame. Save the markers for restore */
		if (infos_len == stack->infos_capacity) {
			stack->infos_capacity = infos_len * 2;
			stack->infos = (FrameDataInfo *) g_realloc (stack->infos, stack->infos_capacity
			                                                              * sizeof (FrameDataInfo));
		}
		stack->infos[infos_len].frame = frame;
		stack->infos[infos_len].frag = current;
		stack->infos[infos_len].pos = current->pos;
		stack->infos_len++;
	}

	if (G_LIKELY (current->pos + size <= current->end)) {
		res = current->pos;
		current->pos += size;
	} else {
		if (current->next && current->next->pos + size <= current->next->end) {
			current = stack->current = current->next;
			current->pos = (guint8 *) &current->data;
		} else {
			FrameDataFragment *tmp = current->next;
			/* avoid linking to be freed fragments, so the GC can't trip over it */
			current->next = NULL;
			mono_compiler_barrier ();
			frame_data_frag_free (tmp);

			current = frame_data_allocator_add_frag (stack, size);
		}
		g_assert (current->pos + size <= current->end);
		res = (gpointer) current->pos;
		current->pos += size;
	}
	mono_compiler_barrier ();
	return res;
}

inline void
frame_data_allocator_pop (FrameDataAllocator *stack, InterpFrame *frame)
{
	int infos_len = stack->infos_len;

	if (infos_len > 0 && stack->infos[infos_len - 1].frame == frame) {
		infos_len--;
		stack->current = stack->infos[infos_len].frag;
		stack->current->pos = stack->infos[infos_len].pos;
		stack->infos_len = infos_len;
	}
}

/*
 * frame_root_code_owner:
 *
 *   Have FRAME hold whatever keeps the code it is about to execute alive.
 *
 * Almost no method has an owner, so this is a load and a predictable branch; the
 * resolve is paid only by the ones that do.
 */
inline void
frame_root_code_owner (InterpFrame *frame)
{
	MonoGCHandle owner = frame->imethod->code_owner;

	frame->code_owner = G_UNLIKELY (owner != NULL) ? mono_method_get_code_owner (owner) : NULL;
}

/*
 * Record when this frame was entered. Ordering interpreter frames is what the
 * unwinder, a stack walk and the resume path all ask for, and an address cannot
 * answer it: a frame lives on the native stack or in the thread's frame stack
 * depending on which of them made it, and the two grow in opposite directions.
 */
inline void
frame_stamp_ordinal (ThreadContext *context, InterpFrame *frame)
{
	frame->ordinal = ++context->next_frame_ordinal;
}

/*
 * reinit_frame:
 *
 *   Reinitialize a frame.
 */
inline void
reinit_frame (InterpFrame *frame, ThreadContext *context, InterpFrame *parent, InterpMethod *imethod, gpointer stack)
{
	frame->parent = parent;
	frame->imethod = imethod;
	frame->stack = (stackval *) stack;
	frame->state.ip = NULL;
	frame_stamp_ordinal (context, frame);
	frame_root_code_owner (frame);
}

} // namespace mono::interp

#endif
