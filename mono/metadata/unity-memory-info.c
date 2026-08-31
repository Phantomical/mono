#include <config.h>
#include <mono/utils/mono-publib.h>

#include "unity-memory-info.h"
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/image.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/gc-internals.h>
#include <mono/utils/mono-conc-hashtable.h>
#include <glib.h>
#include <stdlib.h>

#if HAVE_BOEHM_GC
#include <mono/utils/gc_wrapper.h>
#endif

typedef struct ClassReportContext {
	ClassReportFunc callback;
	void *user_data;
} ClassReportContext;

static void
ReportHashMapClass(gpointer key, gpointer value, gpointer user_data)
{
	ClassReportContext *context = (ClassReportContext *)user_data;
	MonoClass *klass = (MonoClass *)value;
	if (klass->inited)
		context->callback(klass, context->user_data);
}

static void
ReportHashMapListClasses(gpointer key, gpointer value, gpointer user_data)
{
	ClassReportContext *context = (ClassReportContext *)user_data;
	GSList *list = (GSList *)value;

	while (list != NULL) {
		MonoClass *klass = (MonoClass *)list->data;

		if (klass->inited)
			context->callback(klass, context->user_data);

		list = g_slist_next(list);
	}
}

static void
ReportGenericClass(gpointer key, gpointer value, gpointer user_data)
{
	MonoGenericClass *genericClass = (MonoGenericClass *)key;
	ClassReportContext *context = (ClassReportContext *)user_data;

	if (genericClass->cached_class != NULL && genericClass->cached_class->inited)
		context->callback(genericClass->cached_class, context->user_data);
}

// Iterate MonoInternalHashTable similarly to mono_internal_hash_table_apply, but allow passing custom context
static void
IterateMonoInternalHashTable (MonoInternalHashTable *table, gpointer user_data)
{
	ClassReportContext *context = (ClassReportContext *)user_data;
	for (gint i = 0; i < table->size; i++) {
		gpointer head = table->table [i];
		while (head) {
			MonoClass *klass = (MonoClass *)head;

			if (klass->inited)
				context->callback(klass, context->user_data);

			head = *(table->next_value (head));
		}
	}
}

// Report all classes from an assembly
static void
ReportClassesFromAssembly(MonoAssembly *assembly, void *user_data)
{
	MonoImage *image = mono_assembly_get_image_internal(assembly);
	GSList *list;
	ClassReportContext *context = (ClassReportContext*)user_data;

	if (image->dynamic) {
		GHashTableIter iter;
		gpointer key;
		MonoDynamicImage *dynamicImage = (MonoDynamicImage *)image;
		g_hash_table_iter_init(&iter, dynamicImage->typeref);

		while (g_hash_table_iter_next(&iter, &key, NULL)) {
			MonoType *monoType = (MonoType *)key;
			MonoClass *klass = mono_class_from_mono_type_internal(monoType);

			if (klass && klass->inited)
				context->callback(klass, context->user_data);
		}
	}

	// Iterate all classes created with TypeBuilder
	list = image->reflection_info_unregister_classes;
	while (list) {
		MonoClass *klass = (MonoClass *)list->data;

		if (klass && klass->inited)
			context->callback(klass, context->user_data);

		list = list->next;
	}

	// Iterate all initialized image classes
	// Every created class is in the class_cache
	IterateMonoInternalHashTable(&image->class_cache, user_data);

	// Iterate all initialized array classes
	if (image->array_cache)
		g_hash_table_foreach(image->array_cache, ReportHashMapListClasses, user_data);

	// Iterate all initialized single-dimention array classes
	if (image->szarray_cache)
		g_hash_table_foreach(image->szarray_cache, ReportHashMapClass, user_data);

	// Iterate all initialized class pointer classes
	if (image->ptr_cache)
		g_hash_table_foreach(image->ptr_cache, ReportHashMapClass, user_data);
}

// Report all classes in image sets which contain generic instances.
static void
ReportImageSetClasses(MonoImageSet *imageSet, void* user_data)
{
	// Generic class instances
	if (imageSet->gclass_cache)
		mono_conc_hashtable_foreach(imageSet->gclass_cache, ReportGenericClass, user_data);

	// Generic array class instances
	if (imageSet->array_cache)
		g_hash_table_foreach(imageSet->array_cache, ReportHashMapListClasses, user_data);

	// Generic single dimention array (SZArray) class instances
	if (imageSet->szarray_cache)
		g_hash_table_foreach(imageSet->szarray_cache, ReportHashMapClass, user_data);

	// Generic class pointer instances
	if (imageSet->ptr_cache)
		g_hash_table_foreach(imageSet->ptr_cache, ReportHashMapClass, user_data);
}

// Report all initialized classes in the current domain.
MONO_API void
mono_unity_class_for_each(ClassReportFunc callback, void *user_data)
{
	ClassReportContext reportContext;
	reportContext.callback = callback;
	reportContext.user_data = user_data;
	// Report all assembly classes and assembly specific arrays
	mono_domain_assembly_foreach(mono_domain_get(), ReportClassesFromAssembly, &reportContext);
	// Report all image set arrays which include generic classes
	mono_metadata_image_set_foreach(ReportImageSetClasses, &reportContext);
}

#if HAVE_BOEHM_GC

typedef struct CollectMetadataContext
{
	GHashTable *allTypes;
	int currentIndex;
	MonoMetadataSnapshot *metadata;
} CollectMetadataContext;

static void
ContextRecurseClassData (CollectMetadataContext *context, MonoClass *klass)
{
	gpointer orig_key, value;
	gpointer iter = NULL;
	MonoClassField *field = NULL;
	int fieldCount;

	/* use g_hash_table_lookup_extended as it returns boolean to indicate if value was found.
	 * If we use g_hash_table_lookup it returns the value which we were comparing to NULL. The problem is
	 * that 0 is a valid class index and was confusing our logic.
	 */
	if (klass->inited && !g_hash_table_lookup_extended (context->allTypes, klass, &orig_key, &value)) {
		g_hash_table_insert (context->allTypes, klass, GINT_TO_POINTER (context->currentIndex++));

		fieldCount = mono_class_num_fields (klass);

		if (fieldCount > 0) {
			while ((field = mono_class_get_fields_internal (klass, &iter))) {
				MonoClass *fieldKlass = mono_class_from_mono_type_internal (field->type);

				if (fieldKlass != klass)
					ContextRecurseClassData (context, fieldKlass);
			}
		}
	}
}

// Adapts mono_unity_class_for_each's ClassReportFunc callback shape onto ContextRecurseClassData,
// so metadata collection reuses the same class enumeration mono_unity_class_for_each already does.
static void
CollectClassForMetadata (MonoClass *klass, void *user_data)
{
	ContextRecurseClassData ((CollectMetadataContext *)user_data, klass);
}

static int
FindClassIndex (GHashTable *hashTable, MonoClass *klass)
{
	gpointer orig_key, value;

	if (!g_hash_table_lookup_extended (hashTable, klass, &orig_key, &value))
		return -1;

	return GPOINTER_TO_INT (value);
}

static void
AddMetadataType (gpointer key, gpointer value, gpointer user_data)
{
	MonoClass *klass = (MonoClass *)key;

	int index = GPOINTER_TO_INT (value);
	CollectMetadataContext *context = (CollectMetadataContext *)user_data;
	MonoMetadataSnapshot *metadata = context->metadata;
	MonoMetadataType *type = &metadata->types[index];

	if (klass->rank > 0) {
		type->flags = (MonoMetadataTypeFlags) (kArray | (kArrayRankMask & (klass->rank << 16)));
		type->baseOrElementTypeIndex = FindClassIndex (context->allTypes, m_class_get_element_class (klass));
	} else {
		gpointer iter = NULL;
		int fieldCount = 0;
		MonoClassField *field;
		MonoClass *baseClass;
		MonoVTable *vtable;
		void *statics_data;

		type->flags = (klass->valuetype || klass->_byval_arg.type == MONO_TYPE_PTR) ? kValueType : kNone;
		type->fieldCount = 0;
		fieldCount = mono_class_num_fields (klass);
		if (fieldCount > 0) {
			type->fields = g_new (MonoMetadataField, fieldCount);

			while ((field = mono_class_get_fields_internal (klass, &iter))) {
				MonoMetadataField *metaField = &type->fields[type->fieldCount];
				MonoClass *typeKlass = mono_class_from_mono_type_internal (field->type);

				metaField->typeIndex = FindClassIndex (context->allTypes, typeKlass);

				// This will happen if the field's type is not initialized.
				// It's OK to skip it, because it means the field is guaranteed to be null on any object.
				if (metaField->typeIndex == -1)
					continue;

				// literals have no actual storage, and are not relevant in this context.
				if ((field->type->attrs & FIELD_ATTRIBUTE_LITERAL) != 0)
					continue;

				metaField->isStatic = (field->type->attrs & FIELD_ATTRIBUTE_STATIC) != 0;

				metaField->offset = field->offset;
				metaField->name = field->name;
				type->fieldCount++;
			}
		}

		vtable = mono_class_try_get_vtable (mono_domain_get (), klass);
		statics_data = vtable ? mono_vtable_get_static_field_data (vtable) : NULL;

		type->staticsSize = statics_data ? mono_class_data_size (klass) : 0;
		type->statics = NULL;

		if (type->staticsSize > 0) {
			type->statics = g_new0 (uint8_t, type->staticsSize);
			memcpy (type->statics, statics_data, type->staticsSize);
		}

		baseClass = m_class_get_parent (klass);
		type->baseOrElementTypeIndex = baseClass ? FindClassIndex (context->allTypes, baseClass) : -1;
	}

	type->assemblyName = mono_class_get_image (klass)->assembly->aname.name;
	type->name = mono_type_get_name_full (&klass->_byval_arg, MONO_TYPE_NAME_FORMAT_IL);
	type->typeInfoAddress = (uint64_t)klass;
	type->size = (klass->valuetype) != 0 ? (mono_class_instance_size (klass) - sizeof (MonoObject)) : mono_class_instance_size (klass);
}

static void
CollectMetadata (MonoMetadataSnapshot *metadata)
{
	CollectMetadataContext context;

	context.allTypes = g_hash_table_new (NULL, NULL);
	context.currentIndex = 0;
	context.metadata = metadata;

	mono_unity_class_for_each (CollectClassForMetadata, &context);

	metadata->typeCount = g_hash_table_size (context.allTypes);
	metadata->types = g_new0 (MonoMetadataType, metadata->typeCount);

	g_hash_table_foreach (context.allTypes, AddMetadataType, &context);

	g_hash_table_destroy (context.allTypes);
}

static void
MonoMemPoolNumChunksCallback (void *start, void *end, void *user_data)
{
	int *count = (int *)user_data;
	(*count)++;
}

static int
MonoMemPoolNumChunks (MonoMemPool *pool)
{
	int count = 0;
	mono_mempool_foreach_block (pool, MonoMemPoolNumChunksCallback, &count);
	return count;
}

typedef struct SectionIterationContext
{
	MonoManagedMemorySection *currentSection;
} SectionIterationContext;

static void
AllocateMemoryForSection (void *context, void *sectionStart, void *sectionEnd)
{
	ptrdiff_t sectionSize;

	SectionIterationContext *ctx = (SectionIterationContext *)context;
	MonoManagedMemorySection *section = ctx->currentSection;

	section->sectionStartAddress = (uint64_t)sectionStart;
	sectionSize = (uint8_t *)(sectionEnd) - (uint8_t *)(sectionStart);

	section->sectionSize = (uint32_t)(sectionSize);
	section->sectionBytes = g_new (uint8_t, section->sectionSize);

	ctx->currentSection++;
}

// GC_foreach_heap_section additionally reports GC_HEAP_SECTION_TYPE_FREE/PADDING ranges that hold no
// live data; only USED sections are part of the managed heap we're snapshotting.
static void
AllocateMemoryForUsedHeapSection (void *context, void *sectionStart, void *sectionEnd, GC_heap_section_type type)
{
	if (type != GC_HEAP_SECTION_TYPE_USED)
		return;
	AllocateMemoryForSection (context, sectionStart, sectionEnd);
}

static void
CountUsedHeapSection (void *user_data, void *sectionStart, void *sectionEnd, GC_heap_section_type type)
{
	if (type != GC_HEAP_SECTION_TYPE_USED)
		return;
	(*(int *)user_data)++;
}

static int
CountUsedHeapSections (void)
{
	int count = 0;
	GC_foreach_heap_section (&count, CountUsedHeapSection);
	return count;
}

static void
AllocateMemoryForMemPoolChunk (void *chunkStart, void *chunkEnd, void *context)
{
	AllocateMemoryForSection (context, chunkStart, chunkEnd);
}

static void
CopyHeapSection (void *context, void *sectionStart, void *sectionEnd)
{
	SectionIterationContext *ctx = (SectionIterationContext *)(context);
	MonoManagedMemorySection *section = ctx->currentSection;

	g_assert (section->sectionStartAddress == (uint64_t)(sectionStart));
	g_assert (section->sectionSize == (uint8_t *)(sectionEnd) - (uint8_t *)(sectionStart));
	memcpy (section->sectionBytes, sectionStart, section->sectionSize);

	ctx->currentSection++;
}

static void
CopyUsedHeapSection (void *context, void *sectionStart, void *sectionEnd, GC_heap_section_type type)
{
	if (type != GC_HEAP_SECTION_TYPE_USED)
		return;
	CopyHeapSection (context, sectionStart, sectionEnd);
}

static void
CopyMemPoolChunk (void *chunkStart, void *chunkEnd, void *context)
{
	CopyHeapSection (context, chunkStart, chunkEnd);
}

static void
IncrementCountForImageMemPoolNumChunks (MonoImage *image, gpointer *value, void *user_data)
{
	int *count = (int *)user_data;
	(*count) += MonoMemPoolNumChunks (image->mempool);
}

static int
MonoImagesMemPoolNumChunks (GHashTable *monoImages)
{
	int count = 0;

	g_hash_table_foreach (monoImages, (GHFunc)IncrementCountForImageMemPoolNumChunks, &count);
	return count;
}

static void
AllocateMemoryForMemPool (MonoMemPool *pool, void *user_data)
{
	mono_mempool_foreach_block (pool, AllocateMemoryForMemPoolChunk, user_data);
}

static void
AllocateMemoryForImageMemPool (MonoImage *image, gpointer value, void *user_data)
{
	AllocateMemoryForMemPool (image->mempool, user_data);
}

static void
CopyMemPool (MonoMemPool *pool, SectionIterationContext *context)
{
	mono_mempool_foreach_block (pool, CopyMemPoolChunk, context);
}

static void
CopyImageMemPool (MonoImage *image, gpointer value, SectionIterationContext *context)
{
	CopyMemPool (image->mempool, context);
}

static void
AllocateMemoryForImageClassCache (MonoImage *image, gpointer *value, void *user_data)
{
	AllocateMemoryForSection (user_data, image->class_cache.table, ((uint8_t *)image->class_cache.table) + image->class_cache.size * sizeof (gpointer));
}

static void
CopyImageClassCache (MonoImage *image, gpointer value, SectionIterationContext *context)
{
	CopyHeapSection (context, image->class_cache.table, ((uint8_t *)image->class_cache.table) + image->class_cache.size * sizeof (gpointer));
}

static void
IncrementCountForImageSetMemPoolNumChunks (MonoImageSet *imageSet, void *user_data)
{
	int *count = (int *)user_data;
	(*count) += MonoMemPoolNumChunks (imageSet->mempool);
}

static int
MonoImageSetsMemPoolNumChunks (void)
{
	int count = 0;
	mono_metadata_image_set_foreach (IncrementCountForImageSetMemPoolNumChunks, &count);
	return count;
}

static void
AllocateMemoryForImageSetMemPool (MonoImageSet *imageSet, void *user_data)
{
	AllocateMemoryForMemPool (imageSet->mempool, user_data);
}

static void
CopyImageSetMemPool (MonoImageSet *imageSet, void *user_data)
{
	CopyMemPool (imageSet->mempool, user_data);
}

typedef struct
{
	MonoManagedHeap *heap;
	GHashTable *monoImages;
} CaptureHeapInfoData;

static void
CaptureHeapInfo (CaptureHeapInfoData *data)
{
	MonoManagedHeap *heap = data->heap;
	GHashTable *monoImages = data->monoImages;

	MonoDomain *domain = mono_domain_get ();
	MonoDomain *rootDomain = mono_get_root_domain ();
	MonoMemPool *domainPool = mono_domain_memory_manager (domain)->mp;
	MonoMemPool *rootDomainPool = mono_domain_memory_manager (rootDomain)->mp;
	SectionIterationContext iterationContext;

	heap->sectionCount = CountUsedHeapSections ();
	heap->sectionCount += MonoMemPoolNumChunks (rootDomainPool);
	heap->sectionCount += MonoMemPoolNumChunks (domainPool);
	heap->sectionCount += MonoImagesMemPoolNumChunks (monoImages);
	heap->sectionCount += g_hash_table_size (monoImages); // one class_cache bucket-array section per image
	heap->sectionCount += MonoImageSetsMemPoolNumChunks ();

	heap->sections = g_new0 (MonoManagedMemorySection, heap->sectionCount);

	iterationContext.currentSection = heap->sections;

	GC_foreach_heap_section (&iterationContext, AllocateMemoryForUsedHeapSection);
	mono_domain_lock (rootDomain);
	mono_mempool_foreach_block (rootDomainPool, AllocateMemoryForMemPoolChunk, &iterationContext);
	mono_domain_unlock (rootDomain);
	mono_domain_lock (domain);
	mono_mempool_foreach_block (domainPool, AllocateMemoryForMemPoolChunk, &iterationContext);
	mono_domain_unlock (domain);
	g_hash_table_foreach (monoImages, (GHFunc)AllocateMemoryForImageMemPool, &iterationContext);
	g_hash_table_foreach (monoImages, (GHFunc)AllocateMemoryForImageClassCache, &iterationContext);
	mono_metadata_image_set_foreach (AllocateMemoryForImageSetMemPool, &iterationContext);
}

static void
FreeMonoManagedHeap (MonoManagedHeap *heap)
{
	uint32_t i;

	for (i = 0; i < heap->sectionCount; i++)
		g_free (heap->sections[i].sectionBytes);

	g_free (heap->sections);
}

// The difficulty in capturing the managed snapshot is that we need to do quite some work with the world
// stopped, to make sure our snapshot is "valid" and doesn't change as we copy it, but stopping the world
// means we cannot take any lock or allocate. We deal with it like this:
//
// 1) We take note of the amount of heap sections and their sizes, and allocate memory to copy them into.
// 2) We stop the world.
// 3) With the world stopped, we memcpy() the memory from the real heap sections into the copies.
// 4) We start the world again.
static void
CaptureManagedHeap (MonoManagedHeap *heap, GHashTable *monoImages)
{
	MonoDomain *rootDomain = mono_get_root_domain ();
	MonoDomain *domain = mono_domain_get ();
	MonoMemPool *domainPool = mono_domain_memory_manager (domain)->mp;
	MonoMemPool *rootDomainPool = mono_domain_memory_manager (rootDomain)->mp;
	SectionIterationContext iterationContext;
	CaptureHeapInfoData data;

	data.heap = heap;
	data.monoImages = monoImages;

	CaptureHeapInfo (&data);

	iterationContext.currentSection = heap->sections;

	GC_foreach_heap_section (&iterationContext, CopyUsedHeapSection);
	mono_mempool_foreach_block (rootDomainPool, CopyMemPoolChunk, &iterationContext);
	mono_mempool_foreach_block (domainPool, CopyMemPoolChunk, &iterationContext);
	g_hash_table_foreach (monoImages, (GHFunc)CopyImageMemPool, &iterationContext);
	g_hash_table_foreach (monoImages, (GHFunc)CopyImageClassCache, &iterationContext);
	mono_metadata_image_set_foreach (CopyImageSetMemPool, &iterationContext);
}

static void
GCHandleIterationCallback (MonoObject *managedObject, GList **managedObjects)
{
	*managedObjects = g_list_append (*managedObjects, managedObject);
}

static void
CaptureGCHandleTargets (MonoGCHandles *gcHandles)
{
	uint32_t i;
	GList *trackedObjects, *trackedObject;

	trackedObjects = NULL;

	mono_gc_strong_handle_foreach ((GFunc)GCHandleIterationCallback, &trackedObjects);

	gcHandles->trackedObjectCount = (uint32_t)g_list_length (trackedObjects);
	gcHandles->pointersToObjects = (uint64_t *)g_new0 (uint64_t, gcHandles->trackedObjectCount);

	trackedObject = trackedObjects;

	for (i = 0; i < gcHandles->trackedObjectCount; i++) {
		gcHandles->pointersToObjects[i] = (uint64_t)trackedObject->data;
		trackedObject = g_list_next (trackedObject);
	}

	g_list_free (trackedObjects);
}

static void
FillRuntimeInformation (MonoRuntimeInformation *runtimeInfo)
{
	runtimeInfo->pointerSize = (uint32_t)(sizeof (void *));
	runtimeInfo->objectHeaderSize = (uint32_t)(sizeof (MonoObject));
	runtimeInfo->arrayHeaderSize = offsetof (MonoArray, vector);
	runtimeInfo->arraySizeOffsetInHeader = offsetof (MonoArray, max_length);
	runtimeInfo->arrayBoundsOffsetInHeader = offsetof (MonoArray, bounds);
	runtimeInfo->allocationGranularity = (uint32_t)(2 * sizeof (void *));
}

static void
CollectMonoImage (MonoImage *image, GHashTable *monoImages)
{
	int i;

	if (g_hash_table_lookup (monoImages, image) != NULL)
		return;

	g_hash_table_insert (monoImages, image, image);

	if (image->assembly->image != NULL && image != image->assembly->image)
		CollectMonoImage (image->assembly->image, monoImages);

	for (i = 0; i < image->module_count; ++i) {
		MonoImage *moduleImage = image->modules[i];

		if (moduleImage)
			CollectMonoImage (moduleImage, monoImages);
	}
}

static void
CollectMonoImageFromAssembly (MonoAssembly *assembly, void *user_data)
{
	CollectMonoImage (mono_assembly_get_image_internal (assembly), (GHashTable *)user_data);
}

MonoManagedMemorySnapshot *
mono_unity_capture_memory_snapshot (void)
{
	MonoManagedMemorySnapshot *snapshot;
	GHashTable *monoImages;

	GC_disable ();
	GC_stop_world_external ();

	snapshot = g_new0 (MonoManagedMemorySnapshot, 1);
	monoImages = g_hash_table_new (NULL, NULL);

	mono_domain_assembly_foreach (mono_domain_get (), CollectMonoImageFromAssembly, monoImages);

	CollectMetadata (&snapshot->metadata);
	CaptureManagedHeap (&snapshot->heap, monoImages);
	CaptureGCHandleTargets (&snapshot->gcHandles);
	FillRuntimeInformation (&snapshot->runtimeInformation);

	g_hash_table_destroy (monoImages);

	GC_start_world_external ();
	GC_enable ();

	return snapshot;
}

void
mono_unity_free_captured_memory_snapshot (MonoManagedMemorySnapshot *snapshot)
{
	uint32_t i;
	MonoMetadataSnapshot *metadata = &snapshot->metadata;

	FreeMonoManagedHeap (&snapshot->heap);

	g_free (snapshot->gcHandles.pointersToObjects);

	for (i = 0; i < metadata->typeCount; i++) {
		if ((metadata->types[i].flags & kArray) == 0) {
			g_free (metadata->types[i].fields);
			g_free (metadata->types[i].statics);
		}

		g_free (metadata->types[i].name);
	}

	g_free (metadata->types);
	g_free (snapshot);
}

#else

MonoManagedMemorySnapshot *
mono_unity_capture_memory_snapshot (void)
{
	return g_new0 (MonoManagedMemorySnapshot, 1);
}

void
mono_unity_free_captured_memory_snapshot (MonoManagedMemorySnapshot *snapshot)
{
	g_free (snapshot);
}

#endif
