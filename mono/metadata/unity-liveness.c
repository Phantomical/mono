#include <config.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/object.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads-types.h>
#include <mono/utils/atomic.h>
#include <mono/utils/gc_wrapper.h>
#include <mono/utils/mono-error.h>
#include <mono/utils/mono-lazy-init.h>
#include <mono/utils/mono-os-mutex.h>
#include <mono/utils/mono-proclib.h>
#include <mono/utils/mono-threads.h>
#include <mono/utils/w32api.h>

#if defined(HAVE_SGEN_GC)
#include "sgen/sgen-gc.h"
#include "sgen/sgen-client.h"
#elif defined(HAVE_BOEHM_GC)
#include <gc_mark.h>
/* GC_stop_world_external/GC_start_world_external come from mono/utils/gc_wrapper.h's <gc.h> */
#else
#error need to implement liveness GC API
#endif

typedef struct _LivenessState LivenessState;

#define k_block_size (8 * 1024)
#define k_array_elements_per_block ((k_block_size - 3 * sizeof (void*)) / sizeof (gpointer))

typedef struct _custom_array_block custom_array_block;

typedef struct _custom_array_block {
	gpointer *next_item;
	custom_array_block *prev_block;
	custom_array_block *next_block;
	gpointer p_data[k_array_elements_per_block];
} custom_array_block;

typedef struct _custom_block_array_iterator custom_block_array_iterator;

typedef struct _custom_growable_block_array {
	custom_array_block *first_block;
	custom_array_block *current_block;
	custom_block_array_iterator *iterator;
} custom_growable_block_array;

typedef struct _custom_block_array_iterator {
	custom_growable_block_array *array;
	custom_array_block *current_block;
	gpointer *current_position;
} custom_block_array_iterator;


typedef void(*register_object_callback) (gpointer *arr, int size, void *callback_userdata);
typedef void(*WorldStateChanged) ();
typedef void *(*ReallocateArray) (void *ptr, int size, void *callback_userdata);

typedef struct _LivenessWalk LivenessWalk;

/* One thread's share of a traversal. */
typedef struct _LivenessWorker {
	LivenessState *state;
	/* Every object this worker marked, split by whether it derives from
	 * the state's filter. */
	custom_growable_block_array *matched_objects;
	custom_growable_block_array *other_objects;
	custom_growable_block_array *process_array;
	/* Limit recursive traversal to avoid overflowing the stack. */
	guint traverse_depth;
	LivenessWalk *walk;
} LivenessWorker;

struct _LivenessState {
	LivenessWorker *workers;
	int n_workers;

	MonoClass *filter;

	guint initial_alloc_count;

	void *callback_userdata;

	register_object_callback filter_callback;
	ReallocateArray reallocateArray;

	WorldStateChanged on_world_started;

	gboolean heap_validation_skipped_gchandles;
};

struct _LivenessWalk {
	GPtrArray *vtables;
	gint32 n_vtables;
	volatile gint32 next_root;

	mono_mutex_t lock;
	mono_cond_t cond;
	/* Work blocks donated by busy workers. */
	custom_array_block *donated;
	custom_array_block *spare;
	int n_workers;
	int idle;
	/* Workers waiting for work, excluding available donated blocks. */
	volatile gint32 hungry;
	gboolean done;
};

static mono_lazy_init_t liveness_initialized = MONO_LAZY_INIT_STATUS_NOT_INITIALIZED;

/* Serializes every call into a state's ReallocateArray, which the host is not
 * required to make thread-safe. */
static mono_mutex_t liveness_alloc_lock;

static struct {
	mono_mutex_t lock;
	mono_cond_t wake;
	mono_cond_t finished;
	guint32 generation;
	LivenessState *state;
	void (*job) (LivenessWorker *worker);
	int running;
	int n_threads;
} liveness_pool;

static void * liveness_realloc(LivenessState *state, void *ptr, int size)
{
	mono_os_mutex_lock (&liveness_alloc_lock);
	void *result = state->reallocateArray(ptr, size, state->callback_userdata);
	mono_os_mutex_unlock (&liveness_alloc_lock);
	return result;
}

static custom_growable_block_array * block_array_create(LivenessState *state)
{
	custom_growable_block_array *array = g_new0(custom_growable_block_array, 1);
	array->current_block = liveness_realloc(state, NULL, k_block_size);
	array->current_block->prev_block = NULL;
	array->current_block->next_block = NULL;
	array->current_block->next_item = array->current_block->p_data;
	array->first_block = array->current_block;

	array->iterator = g_new0(custom_block_array_iterator, 1);
	array->iterator->array = array;
	array->iterator->current_block = array->first_block;
	array->iterator->current_position = array->first_block->p_data;
	return array;
}

static gboolean block_array_is_empty(custom_growable_block_array *block_array)
{
	return block_array->first_block->next_item == block_array->first_block->p_data;
}

static void block_array_push_back(custom_growable_block_array *block_array, gpointer value, LivenessState *state)
{
	if (block_array->current_block->next_item == block_array->current_block->p_data + k_array_elements_per_block) {
		custom_array_block* new_block = block_array->current_block->next_block;
		if (block_array->current_block->next_block == NULL)
		{
			new_block = liveness_realloc(state, NULL, k_block_size);
			new_block->next_block = NULL;
			new_block->prev_block = block_array->current_block;
			new_block->next_item = new_block->p_data;
			block_array->current_block->next_block = new_block;
		}
		block_array->current_block = new_block;
	}
	*block_array->current_block->next_item++ = value;
}

static gpointer block_array_pop_back(custom_growable_block_array *block_array)
{
	if (block_array->current_block->next_item == block_array->current_block->p_data) {
		if (block_array->current_block->prev_block == NULL)
			return NULL;
		block_array->current_block = block_array->current_block->prev_block;
		block_array->current_block->next_item = block_array->current_block->p_data + k_array_elements_per_block;
	}
	return *--block_array->current_block->next_item;
}

static gpointer block_array_next(custom_growable_block_array *block_array)
{
	custom_block_array_iterator *iterator = block_array->iterator;
	if (iterator->current_position != iterator->current_block->next_item)
		return *iterator->current_position++;
	if (iterator->current_block->next_block == NULL)
		return NULL;
	iterator->current_block = iterator->current_block->next_block;
	iterator->current_position = iterator->current_block->p_data;
	if (iterator->current_position == iterator->current_block->next_item)
		return NULL;
	return *iterator->current_position++;
}

static void block_array_clear(custom_growable_block_array *block_array)
{
	custom_array_block *block = block_array->first_block;
	while (block != NULL) {
		block->next_item = block->p_data;
		block = block->next_block;
	}
	block_array->current_block = block_array->first_block;
}

static void block_array_destroy(custom_growable_block_array *block_array, LivenessState *state)
{
	custom_array_block *block = block_array->first_block;
	while (block != NULL) {
		void *data_block = block;
		block = block->next_block;
		liveness_realloc(state, data_block, 0);
	}
	g_free(block_array->iterator);
	g_free(block_array);
}

/* number of sub elements of an array to process before recursing
 * we take a depth first approach to use stack space rather than re-allocating
 * processing array which requires restarting world to ensure allocator lock is not held
*/
const int kArrayElementsPerChunk = 256;

/* how far we recurse processing array elements before we stop. Prevents stack overflow */
const int kMaxTraverseRecursionDepth = 128;

/* The fewest entries a worker hands to an idle one. */
#define k_min_donation 32

/* How many class_vtable_array entries a worker claims at once. */
#define k_roots_per_claim 64

/* Liveness calculation */
MONO_API LivenessState * mono_unity_liveness_allocate_struct(MonoClass *filter, guint max_count, register_object_callback callback, void *callback_userdata, ReallocateArray reallocateArray);
MONO_API void mono_unity_liveness_stop_gc_world (void);
MONO_API void mono_validate_object_pointer (MonoObject *object);
MONO_API void mono_validate_string_pointer (MonoString *string);
MONO_API void mono_unity_liveness_finalize(LivenessState *state);
MONO_API void mono_unity_liveness_start_gc_world (void);
MONO_API void mono_unity_liveness_free_struct(LivenessState *state);

MONO_API LivenessState * mono_unity_liveness_calculation_begin(MonoClass *filter, guint max_count, register_object_callback callback, void *callback_userdata, WorldStateChanged onWorldStarted, WorldStateChanged onWorldStopped);
MONO_API void mono_unity_liveness_calculation_end(LivenessState *state);

MONO_API void mono_unity_liveness_calculation_from_root(MonoObject *root, LivenessState *state);
MONO_API void mono_unity_liveness_calculation_from_statics(LivenessState *state);
MONO_API void mono_unity_heap_validation_from_statics(LivenessState* state);
MONO_API gboolean mono_unity_heap_validation_gchandles_skipped(LivenessState* state);

#if defined(HAVE_SGEN_GC)
/* Bit 0 of the vtable word is SGen's own forwarding tag (sgen-gc.h), so
 * marking or clearing it outside a stopped world can be read back as a
 * forwarding pointer into the object's own vtable. */
#define ASSERT_WORLD_STOPPED() g_assert (sgen_is_world_stopped ())
#else
#define ASSERT_WORLD_STOPPED() do { } while (0)
#endif

#define VTABLE_WORD(obj) (*(volatile gsize *) &(obj)->vtable)

#define CLEAR_OBJ(obj)                                                  \
	do {                                                                \
		ASSERT_WORLD_STOPPED ();                                       \
		VTABLE_WORD (obj) &= ~(gsize)1;                                \
	} while (0)

#define IS_MARKED(obj) \
	(VTABLE_WORD (obj) & (gsize)1)

#define GET_VTABLE(obj) \
	((MonoVTable *)(VTABLE_WORD (obj) & ~(gsize)1))

/* Sets the mark bit, and returns whether this call is the one that set it. */
static gboolean mark_obj(MonoObject *obj)
{
	ASSERT_WORLD_STOPPED ();
	gpointer unmarked = GET_VTABLE (obj);
	return mono_atomic_cas_ptr ((volatile gpointer *) &obj->vtable, (gpointer) ((gsize) unmarked | 1), unmarked) == unmarked;
}

void mono_filter_objects(LivenessState *state);

static void mono_reset_state(LivenessState *state)
{
	for (int i = 0; i < state->n_workers; i++)
		block_array_clear(state->workers[i].process_array);
}

static gboolean should_process_value(MonoObject *val, MonoClass *filter)
{
	MonoClass *val_class = GET_VTABLE(val)->klass;
	if (filter &&
		!mono_class_has_parent(val_class, filter))
		return FALSE;

	return TRUE;
}

static void mono_validate_array(MonoArray *array, LivenessWorker *worker);
static void mono_validate_object(MonoObject *object, LivenessWorker *worker);
static void mono_traverse_objects(LivenessWorker *worker);
static gboolean mono_add_process_object(MonoObject *object, LivenessWorker *worker);
static gboolean mono_traverse_object_internal(MonoObject *object, gboolean isStruct, MonoClass *klass, LivenessWorker *worker);

#ifdef HAVE_SGEN_GC
static void mono_traverse_generic_object(MonoObject *object, LivenessWorker *worker)
{
	char *start = (char *)object;
	mword desc = sgen_vtable_get_descriptor ((GCVTable)GET_VTABLE (object));

#define HANDLE_PTR(ptr,obj) mono_add_process_object (*(MonoObject **)(ptr), worker)
#include "sgen/sgen-scan-object.h"
#undef HANDLE_PTR
}
#else
/* Visits the words at @base that @desc, a GC_DS_BITMAP descriptor, marks as
 * references. Its highest bit is word 0. */
static void mono_traverse_boehm_bitmap(char *base, gsize desc, LivenessWorker *worker)
{
	MonoObject **slot = (MonoObject **)base;

	for (gsize bitmap = desc & ~(gsize)GC_DS_TAGS; bitmap; bitmap <<= 1, slot++) {
		if (bitmap >> (sizeof (gsize) * 8 - 1))
			mono_add_process_object(*slot, worker);
	}
}

static void mono_traverse_array(MonoArray *array, LivenessWorker *worker)
{
	MonoClass *array_class = GET_VTABLE(&array->obj)->klass;
	MonoClass *element_class = m_class_get_element_class(array_class);
	uintptr_t length = mono_array_length_internal(array);

	if (!m_class_is_valuetype(element_class)) {
		for (uintptr_t i = 0; i < length; i++)
			mono_add_process_object(mono_array_get_fast(array, MonoObject *, i), worker);
		return;
	}

	/* A value type's descriptor describes the boxed value, so it starts one
	 * object header before the element. */
	gsize desc = m_class_is_gc_descr_inited(element_class) ? (gsize)m_class_get_gc_descr(element_class) : 0;
	int element_size = mono_array_element_size(array_class);
	for (uintptr_t i = 0; i < length; i++) {
		char *element = mono_array_addr_with_size_fast(array, element_size, i);
		if ((desc & GC_DS_TAGS) == GC_DS_BITMAP)
			mono_traverse_boehm_bitmap(element - sizeof (MonoObject), desc, worker);
		else
			mono_traverse_object_internal((MonoObject *)element, TRUE, element_class, worker);
	}
}

static void mono_traverse_generic_object(MonoObject *object, LivenessWorker *worker)
{
	MonoVTable *vtable = GET_VTABLE(object);
	gsize desc = (gsize)vtable->gc_descr;

	if (vtable->klass->rank)
		mono_traverse_array((MonoArray *)object, worker);
	else if ((desc & GC_DS_TAGS) == GC_DS_BITMAP)
		mono_traverse_boehm_bitmap((char *)object, desc, worker);
	else
		mono_traverse_object_internal(object, FALSE, vtable->klass, worker);
}
#endif

static void mono_traverse_and_validate_generic_object(MonoObject *object, LivenessWorker *worker)
{
	if (GET_VTABLE(object)->klass->rank)
		mono_validate_array((MonoArray*)object, worker);
	else
		mono_validate_object(object, worker);
}

static void validate_object_value(MonoObject *val, MonoType *storageType)
{
	if (val && storageType->type == MONO_TYPE_CLASS) {
		MonoClass *storageClass = storageType->data.klass;
		MonoClass *valClass = GET_VTABLE(val)->klass;
		if (mono_class_is_interface(storageClass)) {
			int found = 0;
			for (int i = 0; i < valClass->interface_offsets_count; ++i) {
				if (valClass->interfaces_packed[i] == storageClass) {
					found = TRUE;
					break;
				}
			}
			g_assert(found);
		}
		else {
			int res = mono_class_has_parent_fast(valClass, storageClass);
			g_assert(res);
		}
	}
}

static gboolean mono_add_process_object(MonoObject *object, LivenessWorker *worker)
{
	if (!object || IS_MARKED(object))
		return FALSE;

	gboolean has_references = GET_VTABLE(object)->klass->has_references;
	gboolean matched;
	if (has_references) {
		if (!mark_obj(object))
			return FALSE;
		matched = should_process_value(object, worker->state->filter);
	} else {
		if (!should_process_value(object, worker->state->filter) || !mark_obj(object))
			return FALSE;
		matched = TRUE;
	}

	block_array_push_back(matched ? worker->matched_objects : worker->other_objects, object, worker->state);
	/* Objects without references do not need to be traversed. */
	if (has_references) {
		block_array_push_back(worker->process_array, object, worker->state);
		return TRUE;
	}

	return FALSE;
}

MONO_API void mono_validate_object_pointer (MonoObject *object)
{
	if (object) {
		MonoVTable *vtable = NULL;
		MonoClass *klass = NULL;
		const char *name = NULL;

		vtable = object->vtable;
		klass = vtable->klass;
		name = klass->name;

		g_assert(vtable);
		g_assert(klass);
		g_assert(name);
	}
}

MONO_API void mono_validate_string_pointer(MonoString *string)
{
	mono_validate_object_pointer(&string->object);
}

static gboolean mono_add_and_validate_object(MonoObject *object, LivenessWorker *worker)
{
	if (object) {
		MonoVTable *vtable = NULL;
		MonoClass *klass = NULL;
		const char *name = NULL;
		vtable = GET_VTABLE(object);
		klass = vtable->klass;
		name = klass->name;

		g_assert(vtable);
		g_assert(klass);
		g_assert(name);

		return mono_add_process_object(object, worker);
	}

	return FALSE;
}

static gboolean mono_field_can_contain_references(MonoClassField *field)
{
	if (MONO_TYPE_ISSTRUCT(field->type))
		return TRUE;
	if (field->type->attrs & FIELD_ATTRIBUTE_LITERAL)
		return FALSE;
	if (field->type->type == MONO_TYPE_STRING)
		return FALSE;
	return MONO_TYPE_IS_REFERENCE(field->type);
}

/* mono_field_get_value_internal () asserts in a checked build that the calling
 * thread is attached, and a pool thread is not. */
static MonoObject * reference_field_value(MonoObject *object, MonoClassField *field)
{
	return *(MonoObject **)((char *)object + field->offset);
}

static gboolean mono_traverse_object_internal(MonoObject *object, gboolean isStruct, MonoClass *klass, LivenessWorker *worker)
{
	guint32 i;
	MonoClassField *field;
	MonoClass *p;
	gboolean added_objects = FALSE;

	if (!isStruct && mono_class_has_parent_fast(klass, mono_defaults.real_proxy_class)) return FALSE;

	g_assert(object);

	/* Value-type field offsets include the object header. */
	if (isStruct)
		object--;

	for (p = klass; p != NULL; p = p->parent) {
		if (p->size_inited == 0)
			continue;
		for (i = 0; i < mono_class_get_field_count(p); i++) {
			field = &p->fields[i];
			if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
				continue;

			if (!mono_field_can_contain_references(field))
				continue;

			if (MONO_TYPE_ISSTRUCT(field->type)) {
				char *offseted = (char *)object;
				offseted += field->offset;
				if (field->type->type == MONO_TYPE_GENERICINST) {
					g_assert(field->type->data.generic_class->cached_class);
					added_objects |= mono_traverse_object_internal((MonoObject *)offseted, TRUE, field->type->data.generic_class->cached_class, worker);
				}
				else
					added_objects |= mono_traverse_object_internal((MonoObject *)offseted, TRUE, field->type->data.klass, worker);
				continue;
			}

			if (field->offset == -1) {
				g_assert_not_reached();
			}
			else {
				MonoObject *val = reference_field_value(object, field);
				added_objects |= mono_add_process_object(val, worker);
			}
		}
	}

	return added_objects;
}

static gboolean mono_validate_object_internal(MonoObject *object, gboolean isStruct, MonoClass *klass, LivenessWorker *worker)
{
	int i;
	MonoClassField* field;
	MonoClass* p;
	gboolean added_objects = FALSE;

	if (!isStruct && mono_class_has_parent_fast(klass, mono_defaults.real_proxy_class)) return FALSE;

	g_assert(object);

	/* Value-type field offsets include the object header. */
	if (isStruct)
		object--;

	for (p = klass; p != NULL; p = p->parent) {
		if (p->size_inited == 0)
			continue;
		for (i = 0; i < mono_class_get_field_count(p); i++) {
			field = &p->fields[i];
			if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
				continue;

			if (!mono_field_can_contain_references(field))
				continue;

			if (MONO_TYPE_ISSTRUCT(field->type)) {
				char* offseted = (char*)object;
				offseted += field->offset;
				if (field->type->type == MONO_TYPE_GENERICINST)
				{
					g_assert (field->type->data.generic_class->cached_class);
					added_objects |= mono_validate_object_internal((MonoObject*)offseted, TRUE, field->type->data.generic_class->cached_class, worker);
				}
				else
					added_objects |= mono_validate_object_internal((MonoObject*)offseted, TRUE, field->type->data.klass, worker);
				continue;
			}

			if (field->offset == -1) {
				g_assert_not_reached();
			}
			else {
				MonoObject* val = reference_field_value(object, field);
				added_objects |= mono_add_and_validate_object(val, worker);
				validate_object_value(val, field->type);
			}
		}
	}

	return added_objects;
}

static void mono_validate_object(MonoObject *object, LivenessWorker *worker)
{
	mono_validate_object_internal(object, FALSE, GET_VTABLE(object)->klass, worker);
}

/* Moves entries from the top of the worker's process_array to walk->donated. */
static void liveness_donate(LivenessWorker *worker)
{
	LivenessWalk *walk = worker->walk;
	custom_array_block *top = worker->process_array->current_block;
	gsize count = top->next_item - top->p_data;

	if (top == worker->process_array->first_block)
		count /= 2;
	if (count < k_min_donation)
		return;

	mono_os_mutex_lock (&walk->lock);
	if (walk->hungry <= 0) {
		mono_os_mutex_unlock (&walk->lock);
		return;
	}
	custom_array_block *chunk = walk->spare;
	if (chunk)
		walk->spare = chunk->next_block;
	else
		chunk = liveness_realloc(worker->state, NULL, k_block_size);
	top->next_item -= count;
	memcpy (chunk->p_data, top->next_item, count * sizeof (gpointer));
	chunk->next_item = chunk->p_data + count;
	chunk->next_block = walk->donated;
	walk->donated = chunk;
	mono_atomic_dec_i32 (&walk->hungry);
	mono_os_cond_signal (&walk->cond);
	mono_os_mutex_unlock (&walk->lock);
}

static void mono_traverse_objects(LivenessWorker *worker)
{
	MonoObject *object = NULL;
	LivenessWalk *walk = worker->walk;

	worker->traverse_depth++;
	while (!block_array_is_empty(worker->process_array)) {
		object = block_array_pop_back(worker->process_array);
		mono_traverse_generic_object(object, worker);
		if (walk && mono_atomic_load_i32_relaxed (&walk->hungry) > 0)
			liveness_donate(worker);
	}
	worker->traverse_depth--;
}

static void mono_traverse_and_validate_objects(LivenessWorker *worker)
{
	MonoObject* object = NULL;

	worker->traverse_depth++;
	while (!block_array_is_empty(worker->process_array)) {
		object = block_array_pop_back(worker->process_array);
		mono_traverse_and_validate_generic_object(object, worker);
	}
	worker->traverse_depth--;
}

static gboolean should_traverse_objects(size_t index, LivenessWorker *worker)
{
	/* Periodically drain the work list while processing large arrays. */
	return ((index + 1) & (kArrayElementsPerChunk - 1)) == 0 &&
		worker->traverse_depth < kMaxTraverseRecursionDepth;
}

static void mono_validate_array(MonoArray *array, LivenessWorker *worker)
{
	size_t i = 0;
	gboolean has_references;
	MonoObject *object = (MonoObject*)array;
	MonoClass *element_class;
	size_t elementClassSize;
	size_t array_length;

	g_assert(object);



	element_class = GET_VTABLE(object)->klass->element_class;
	has_references = !m_class_is_valuetype(element_class);
	g_assert(element_class->size_inited != 0);

	for (i = 0; i < mono_class_get_field_count(element_class); i++)	{
		has_references |= mono_field_can_contain_references(&element_class->fields[i]);
	}

	if (!has_references)
		return;

	array_length = mono_array_length_internal(array);
	if (element_class->valuetype) {
		size_t items_processed = 0;
		elementClassSize = mono_class_array_element_size(element_class);
		for (i = 0; i < array_length; i++) {
			MonoObject *object = (MonoObject*)mono_array_addr_with_size_internal(array, elementClassSize, i);
			if (mono_validate_object_internal(object, 1, element_class, worker))
				items_processed++;

			if (should_traverse_objects(items_processed, worker))
				mono_traverse_and_validate_objects(worker);
		}
	}
	else {
		size_t items_processed = 0;
		for (i = 0; i < array_length; i++) {
			MonoObject *val = mono_array_get(array, MonoObject*, i);
			if (mono_add_and_validate_object(val, worker))
				items_processed++;

			validate_object_value(val, &element_class->_byval_arg);

			if (should_traverse_objects (items_processed, worker))
				mono_traverse_and_validate_objects(worker);
		}
	}
}

void mono_filter_objects(LivenessState *state)
{
	gpointer filtered_objects[64];
	gint num_objects = 0;

	for (int i = 0; i < state->n_workers; i++) {
		custom_growable_block_array *matched_objects = state->workers[i].matched_objects;
		gpointer value = block_array_next(matched_objects);
		while (value != NULL) {
			filtered_objects[num_objects++] = value;
			if (num_objects == 64) {
				state->filter_callback(filtered_objects, 64, state->callback_userdata);
				num_objects = 0;
			}
			value = block_array_next(matched_objects);
		}
	}

	if (num_objects != 0)
		state->filter_callback(filtered_objects, num_objects, state->callback_userdata);
}

static void mono_add_static_roots(MonoVTable *vtable, LivenessWorker *worker)
{
	MonoClass *klass = vtable->klass;
	MonoClassField *field;
	guint32 j;

	if (!klass)
		return;
	if (!klass->has_static_refs)
		return;
	if (klass->image == mono_defaults.corlib)
		return;
	if (klass->size_inited == 0)
		return;
	for (j = 0; j < mono_class_get_field_count(klass); j++) {
		field = &klass->fields[j];
		if (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC))
			continue;
		if (!mono_field_can_contain_references(field))
			continue;
		/* Special statics are not stored in the vtable data block. */
		if (field->offset == -1)
			continue;

		char *offseted = (char *)mono_vtable_get_static_field_data(vtable);
		offseted += field->offset;

		if (MONO_TYPE_ISSTRUCT(field->type)) {
			if (field->type->type == MONO_TYPE_GENERICINST) {
				g_assert(field->type->data.generic_class->cached_class);
				mono_traverse_object_internal((MonoObject *)offseted, TRUE, field->type->data.generic_class->cached_class, worker);
			}
			else
				mono_traverse_object_internal((MonoObject *)offseted, TRUE, field->type->data.klass, worker);
		}
		else {
			MonoObject* val = *(MonoObject**)offseted;
			if (val)
				mono_add_process_object(val, worker);
		}
	}
}

/* Takes a block another worker donated, or returns FALSE once every worker
 * is waiting here with nothing left to donate. */
static gboolean liveness_take(LivenessWorker *worker)
{
	LivenessWalk *walk = worker->walk;
	custom_array_block *chunk = NULL;

	mono_os_mutex_lock (&walk->lock);
	while (!walk->done) {
		chunk = walk->donated;
		if (chunk) {
			walk->donated = chunk->next_block;
			mono_atomic_inc_i32 (&walk->hungry);
			break;
		}
		if (walk->idle + 1 == walk->n_workers) {
			walk->done = TRUE;
			mono_os_cond_broadcast (&walk->cond);
			break;
		}
		walk->idle++;
		mono_atomic_inc_i32 (&walk->hungry);
		mono_os_cond_wait (&walk->cond, &walk->lock);
		mono_atomic_dec_i32 (&walk->hungry);
		walk->idle--;
	}
	if (chunk) {
		for (gpointer *item = chunk->p_data; item != chunk->next_item; item++)
			block_array_push_back(worker->process_array, *item, worker->state);
		chunk->next_block = walk->spare;
		walk->spare = chunk;
	}
	mono_os_mutex_unlock (&walk->lock);

	return chunk != NULL;
}

static void liveness_walk_run(LivenessWorker *worker)
{
	LivenessWalk *walk = worker->walk;

	for (;;) {
		gint32 start = mono_atomic_fetch_add_i32 (&walk->next_root, k_roots_per_claim);
		if (start >= walk->n_vtables)
			break;
		gint32 end = MIN (start + k_roots_per_claim, walk->n_vtables);
		for (gint32 i = start; i < end; i++)
			mono_add_static_roots((MonoVTable *)g_ptr_array_index(walk->vtables, i), worker);
		mono_traverse_objects(worker);
	}

	do
		mono_traverse_objects(worker);
	while (liveness_take(worker));
}

static mono_thread_start_return_t WINAPI liveness_pool_thread(gpointer arg)
{
	int index = GPOINTER_TO_INT (arg);
	guint32 seen = 0;

#ifdef HOST_WIN32
	mono_thread_set_name_windows (GetCurrentThread (), L"Mono GC Worker");
#else
	mono_native_thread_set_name (mono_native_thread_id_get (), "Mono GC Worker");
#endif

	mono_os_mutex_lock (&liveness_pool.lock);
	for (;;) {
		while (liveness_pool.generation == seen)
			mono_os_cond_wait (&liveness_pool.wake, &liveness_pool.lock);
		seen = liveness_pool.generation;
		LivenessState *state = liveness_pool.state;
		void (*job) (LivenessWorker *worker) = liveness_pool.job;
		if (index >= state->n_workers)
			continue;
		mono_os_mutex_unlock (&liveness_pool.lock);

		job(&state->workers[index]);

		mono_os_mutex_lock (&liveness_pool.lock);
		if (--liveness_pool.running == 0)
			mono_os_cond_signal (&liveness_pool.finished);
	}
	return 0;
}

/* Runs @job on every worker of @state, workers [0] on the calling thread and
 * the rest on the pool, and returns once all of them have. */
static void liveness_pool_run(LivenessState *state, void (*job) (LivenessWorker *worker))
{
	if (state->n_workers == 1) {
		job(&state->workers[0]);
		return;
	}

	mono_os_mutex_lock (&liveness_pool.lock);
	liveness_pool.state = state;
	liveness_pool.job = job;
	liveness_pool.running = state->n_workers - 1;
	liveness_pool.generation++;
	mono_os_cond_broadcast (&liveness_pool.wake);
	mono_os_mutex_unlock (&liveness_pool.lock);

	job(&state->workers[0]);

	mono_os_mutex_lock (&liveness_pool.lock);
	while (liveness_pool.running)
		mono_os_cond_wait (&liveness_pool.finished, &liveness_pool.lock);
	mono_os_mutex_unlock (&liveness_pool.lock);
}

/* Create the unmanaged pool before stopping the world: thread creation can
 * acquire libc locks held by a suspended thread. */
static void liveness_initialize(void)
{
	mono_os_mutex_init (&liveness_alloc_lock);
	mono_os_mutex_init (&liveness_pool.lock);
	mono_os_cond_init (&liveness_pool.wake);
	mono_os_cond_init (&liveness_pool.finished);

	int threads = MIN (mono_cpu_count (), 8);
	char *requested = g_getenv ("MONO_UNITY_LIVENESS_THREADS");
	if (requested) {
		threads = atoi (requested);
		g_free (requested);
	}

	for (int i = 1; i < threads; i++) {
		MonoNativeThreadId tid;
		if (!mono_native_thread_create (&tid, liveness_pool_thread, GINT_TO_POINTER (i)))
			break;
		liveness_pool.n_threads++;
	}
}

/**
 * mono_unity_liveness_calculation_from_statics:
 *
 * Returns an array of MonoObject* that are reachable from the static roots
 * in the current domain and derive from @filter (if not NULL).
 */
void mono_unity_liveness_calculation_from_statics(LivenessState *liveness_state)
{
	MonoDomain *domain = mono_domain_get();
	MonoMemoryManager* memory_manager = mono_domain_memory_manager(domain);
	LivenessWalk walk;
	int i;

	mono_reset_state(liveness_state);

	memset (&walk, 0, sizeof (walk));
	walk.vtables = memory_manager->class_vtable_array;
	walk.n_vtables = walk.vtables->len;
	walk.n_workers = liveness_state->n_workers;
	mono_os_mutex_init (&walk.lock);
	mono_os_cond_init (&walk.cond);
	for (i = 0; i < liveness_state->n_workers; i++)
		liveness_state->workers[i].walk = &walk;

	liveness_pool_run(liveness_state, liveness_walk_run);

	for (i = 0; i < liveness_state->n_workers; i++)
		liveness_state->workers[i].walk = NULL;
	while (walk.spare) {
		custom_array_block *block = walk.spare;
		walk.spare = block->next_block;
		liveness_realloc(liveness_state, block, 0);
	}
	mono_os_cond_destroy (&walk.cond);
	mono_os_mutex_destroy (&walk.lock);

	//Filter objects and call callback to register found objects
	mono_filter_objects(liveness_state);
}

#if HAVE_BOEHM_GC
static void gchandle_process(void *data, void *user_data)
{
	MonoObject *target = data;
	LivenessWorker *worker = user_data;

	mono_add_and_validate_object(target, worker);
}

extern void
mono_gc_strong_handle_foreach(GFunc func, gpointer user_data);
#endif

static void
foreach_thread_static_field (gpointer key, gpointer value, gpointer user_data)
{
	MonoClassField *field = key;
	guint32 offset = GPOINTER_TO_UINT(value);
	LivenessWorker *worker = user_data;

	if (!mono_field_can_contain_references(field))
		return;

	if (MONO_TYPE_ISSTRUCT(field->type))
		return;

	MonoInternalThread *thread;

	thread = mono_thread_internal_current();

	gpointer data = mono_get_special_static_data_for_thread(thread, offset);

	MonoObject *val = *(MonoObject**)data;

	if (val) {
		mono_add_and_validate_object(val, worker);
		validate_object_value(val, field->type);
	}
}

void mono_unity_heap_validation_from_statics(LivenessState *liveness_state)
{
	int i, j;
	MonoDomain *domain = mono_domain_get();
	MonoMemoryManager* memory_manager = mono_domain_memory_manager(domain);
	LivenessWorker *worker = &liveness_state->workers[0];

	mono_reset_state(liveness_state);

#if HAVE_BOEHM_GC
	liveness_state->heap_validation_skipped_gchandles = FALSE;
	mono_gc_strong_handle_foreach(gchandle_process, worker);
#else
	// SGen has no mono_gc_strong_handle_foreach, so this skips the gchandle
	// walk. The thread-static and class-static walks below still run.
	liveness_state->heap_validation_skipped_gchandles = TRUE;
#endif

	g_hash_table_foreach(domain->special_static_fields, foreach_thread_static_field, worker);

	for (i = 0; i < memory_manager->class_vtable_array->len; ++i) {
		MonoVTable *vtable = (MonoVTable*)g_ptr_array_index(memory_manager->class_vtable_array, i);
		MonoClass *klass = vtable->klass;
		MonoClassField *field;
		if (!klass)
			continue;
		if (!klass->has_static_refs)
			continue;
		if (klass->image == mono_defaults.corlib)
			continue;
		if (klass->size_inited == 0)
			continue;
		for (j = 0; j < mono_class_get_field_count(klass); j++)	{
			field = &klass->fields[j];
			if (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC))
				continue;
			if (!mono_field_can_contain_references(field))
				continue;
			// shortcut check for special statics
			if (field->offset == -1)
				continue;
			if (field->type->attrs & FIELD_ATTRIBUTE_LITERAL)
				continue;

			if (MONO_TYPE_ISSTRUCT(field->type)) {
				char* offseted = (char*)mono_vtable_get_static_field_data(vtable);
				offseted += field->offset;
				if (field->type->type == MONO_TYPE_GENERICINST) {
					g_assert(field->type->data.generic_class->cached_class);
					mono_validate_object_internal((MonoObject*)offseted, TRUE, field->type->data.generic_class->cached_class, worker);
				}
				else {
					mono_validate_object_internal((MonoObject*)offseted, TRUE, field->type->data.klass, worker);
				}
			}
			else {
				MonoObject* val = NULL;

				char* offseted = (char*)mono_vtable_get_static_field_data(vtable);
				offseted += field->offset;
				val = *((MonoObject**)offseted);

				if (val)
					mono_add_and_validate_object(val, worker);
			}
		}
	}
	mono_traverse_and_validate_objects(worker);
	//Filter objects and call callback to register found objects
	//mono_filter_objects (liveness_state);
}

/**
 * mono_unity_heap_validation_gchandles_skipped:
 *
 * Whether the gchandle root set was left out of the most recent
 * mono_unity_heap_validation_from_statics() call on @state, which SGen does
 * because it has no mono_gc_strong_handle_foreach().
 */
gboolean mono_unity_heap_validation_gchandles_skipped(LivenessState* state)
{
	return state->heap_validation_skipped_gchandles;
}

/**
 * mono_unity_liveness_calculation_from_root:
 *
 * Returns an array of MonoObject* that are reachable from @root
 * in the current domain and derive from @filter (if not NULL).
 */
void mono_unity_liveness_calculation_from_root(MonoObject *root, LivenessState *liveness_state)
{
	LivenessWorker *worker = &liveness_state->workers[0];

	mono_reset_state(liveness_state);

	block_array_push_back(worker->process_array, root, liveness_state);

	mono_traverse_objects(worker);

	//Filter objects and call callback to register found objects
	mono_filter_objects(liveness_state);
}

LivenessState * mono_unity_liveness_allocate_struct(MonoClass *filter, guint max_count, register_object_callback callback, void *callback_userdata, ReallocateArray reallocateArray)
{
	LivenessState *state = NULL;

	mono_lazy_initialize (&liveness_initialized, liveness_initialize);

	state = g_new0(LivenessState, 1);

	state->filter = filter;

	state->callback_userdata = callback_userdata;
	state->filter_callback = callback;
	state->reallocateArray = reallocateArray;

	state->n_workers = 1 + liveness_pool.n_threads;
	state->workers = g_new0(LivenessWorker, state->n_workers);
	for (int i = 0; i < state->n_workers; i++) {
		LivenessWorker *worker = &state->workers[i];
		worker->state = state;
		worker->matched_objects = block_array_create(state);
		worker->other_objects = block_array_create(state);
		worker->process_array = block_array_create(state);
	}

	return state;
}

static void clear_marks(custom_growable_block_array *objects)
{
	for (custom_array_block *block = objects->first_block; block != NULL; block = block->next_block) {
		for (gpointer *item = block->p_data; item != block->next_item; item++)
			CLEAR_OBJ((MonoObject *)*item);
	}
}

static void liveness_clear_marks(LivenessWorker *worker)
{
	clear_marks(worker->matched_objects);
	clear_marks(worker->other_objects);
}

void mono_unity_liveness_finalize(LivenessState *state)
{
	liveness_pool_run(state, liveness_clear_marks);
}

void mono_unity_liveness_free_struct(LivenessState *state)
{
	//cleanup the liveness_state
	for (int i = 0; i < state->n_workers; i++) {
		block_array_destroy(state->workers[i].matched_objects, state);
		block_array_destroy(state->workers[i].other_objects, state);
		block_array_destroy(state->workers[i].process_array, state);
	}
	g_free(state->workers);
	g_free(state);
}

void mono_unity_liveness_stop_gc_world (void)
{
#if defined(HAVE_SGEN_GC)
	/* sgen_stop_world()/sgen_restart_world() require the GC lock held for
	 * as long as the world stays stopped, the same contract
	 * mono_gc_stop_world()/mono_gc_restart_world() keep in sgen-mono.c. */
	sgen_gc_lock ();
	sgen_stop_world (0, FALSE);
	/* A stopped world leaves the concurrent mark and sweep workers running,
	 * and they read the vtable word the walk marks. */
	sgen_finish_concurrent_work ("unity liveness", FALSE);
#elif defined(HAVE_BOEHM_GC)
	GC_stop_world_external ();
#else
#error need to implement liveness GC API
#endif
}

void mono_unity_liveness_start_gc_world (void)
{
#if defined(HAVE_SGEN_GC)
	sgen_restart_world (0, FALSE);
	sgen_gc_unlock ();
#elif defined(HAVE_BOEHM_GC)
	GC_start_world_external ();
#else
#error need to implement liveness GC API
#endif
}

/*
 * Unity's scripting_liveness_calculation_begin() tail-calls straight into this
 * function rather than going through mono_unity_liveness_allocate_struct(), so
 * there's no caller-supplied ReallocateArray on this path; back the block
 * array with g_malloc/g_free instead.
 */
static void * default_liveness_reallocate_array(void *ptr, int size, void *callback_userdata)
{
	if (size == 0) {
		g_free(ptr);
		return NULL;
	}
	return g_malloc(size);
}

LivenessState * mono_unity_liveness_calculation_begin(MonoClass *filter, guint max_count, register_object_callback callback, void *callback_userdata, WorldStateChanged onWorldStarted, WorldStateChanged onWorldStopped)
{
	LivenessState *state = mono_unity_liveness_allocate_struct(filter, max_count, callback, callback_userdata, default_liveness_reallocate_array);
	state->on_world_started = onWorldStarted;
	mono_unity_liveness_stop_gc_world();
	if (onWorldStopped)
		onWorldStopped();
	// no allocations can happen beyond this point
	return state;
}

void mono_unity_liveness_calculation_end(LivenessState *state)
{
	WorldStateChanged on_world_started = state->on_world_started;
	mono_unity_liveness_finalize(state);
	mono_unity_liveness_start_gc_world();
	if (on_world_started)
		on_world_started();
	mono_unity_liveness_free_struct(state);
}
