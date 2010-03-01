/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Dalvik.h"
#include "alloc/clz.h"
#include "alloc/HeapBitmap.h"
#include "alloc/HeapInternal.h"
#include "alloc/HeapSource.h"
#include "alloc/MarkSweep.h"
#include <limits.h>     // for ULONG_MAX
#include <sys/mman.h>   // for madvise(), mmap()
#include <cutils/ashmem.h>
#include <errno.h>

#define GC_DEBUG_PARANOID   2
#define GC_DEBUG_BASIC      1
#define GC_DEBUG_OFF        0
#define GC_DEBUG(l)         (GC_DEBUG_LEVEL >= (l))

#if 1
#define GC_DEBUG_LEVEL      GC_DEBUG_PARANOID
#else
#define GC_DEBUG_LEVEL      GC_DEBUG_OFF
#endif

#define VERBOSE_GC          0

#define GC_LOG_TAG      LOG_TAG "-gc"

#if LOG_NDEBUG
#define LOGV_GC(...)    ((void)0)
#define LOGD_GC(...)    ((void)0)
#else
#define LOGV_GC(...)    LOG(LOG_VERBOSE, GC_LOG_TAG, __VA_ARGS__)
#define LOGD_GC(...)    LOG(LOG_DEBUG, GC_LOG_TAG, __VA_ARGS__)
#endif

#if VERBOSE_GC
#define LOGVV_GC(...)   LOGV_GC(__VA_ARGS__)
#else
#define LOGVV_GC(...)   ((void)0)
#endif

#define LOGI_GC(...)    LOG(LOG_INFO, GC_LOG_TAG, __VA_ARGS__)
#define LOGW_GC(...)    LOG(LOG_WARN, GC_LOG_TAG, __VA_ARGS__)
#define LOGE_GC(...)    LOG(LOG_ERROR, GC_LOG_TAG, __VA_ARGS__)

#define LOG_SCAN(...)   LOGV_GC("SCAN: " __VA_ARGS__)
#define LOG_MARK(...)   LOGV_GC("MARK: " __VA_ARGS__)
#define LOG_SWEEP(...)  LOGV_GC("SWEEP: " __VA_ARGS__)
#define LOG_REF(...)    LOGV_GC("REF: " __VA_ARGS__)

#define LOGV_SCAN(...)  LOGVV_GC("SCAN: " __VA_ARGS__)
#define LOGV_MARK(...)  LOGVV_GC("MARK: " __VA_ARGS__)
#define LOGV_SWEEP(...) LOGVV_GC("SWEEP: " __VA_ARGS__)
#define LOGV_REF(...)   LOGVV_GC("REF: " __VA_ARGS__)

#define ALIGN_UP_TO_PAGE_SIZE(p) \
    (((size_t)(p) + (SYSTEM_PAGE_SIZE - 1)) & ~(SYSTEM_PAGE_SIZE - 1))

/* Do not cast the result of this to a boolean; the only set bit
 * may be > 1<<8.
 */
static inline long isMarked(const void *obj, const GcMarkContext *ctx)
        __attribute__((always_inline));
static inline long isMarked(const void *obj, const GcMarkContext *ctx)
{
    return dvmHeapBitmapIsObjectBitSet(ctx->bitmap, obj);
}

static bool
createMarkStack(GcMarkStack *stack)
{
    const Object **limit;
    size_t size;
    int fd, err;

    /* Create a stack big enough for the worst possible case,
     * where the heap is perfectly full of the smallest object.
     * TODO: be better about memory usage; use a smaller stack with
     *       overflow detection and recovery.
     */
    size = dvmHeapSourceGetIdealFootprint() * sizeof(Object*) /
            (sizeof(Object) + HEAP_SOURCE_CHUNK_OVERHEAD);
    size = ALIGN_UP_TO_PAGE_SIZE(size);
    fd = ashmem_create_region("dalvik-heap-markstack", size);
    if (fd < 0) {
        LOGE_GC("Could not create %d-byte ashmem mark stack: %s\n",
            size, strerror(errno));
        return false;
    }
    limit = (const Object **)mmap(NULL, size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE, fd, 0);
    err = errno;
    close(fd);
    if (limit == MAP_FAILED) {
        LOGE_GC("Could not mmap %d-byte ashmem mark stack: %s\n",
            size, strerror(err));
        return false;
    }

    memset(stack, 0, sizeof(*stack));
    stack->limit = limit;
    stack->base = (const Object **)((uintptr_t)limit + size);
    stack->top = stack->base;

    return true;
}

static void
destroyMarkStack(GcMarkStack *stack)
{
    munmap((char *)stack->limit,
            (uintptr_t)stack->base - (uintptr_t)stack->limit);
    memset(stack, 0, sizeof(*stack));
}

#define MARK_STACK_PUSH(stack, obj) \
    do { \
        *--(stack).top = (obj); \
    } while (false)

bool
dvmHeapBeginMarkStep()
{
    GcMarkContext *mc = &gDvm.gcHeap->markContext;

    if (!createMarkStack(&mc->stack)) {
        return false;
    }
    mc->finger = NULL;
    return true;
}

static long setAndReturnMarkBit(GcMarkContext *ctx, const void *obj)
        __attribute__((always_inline));
static long
setAndReturnMarkBit(GcMarkContext *ctx, const void *obj)
{
    return dvmHeapBitmapSetAndReturnObjectBit(ctx->bitmap, obj);
}

static void _markObjectNonNullCommon(const Object *obj, GcMarkContext *ctx,
        bool checkFinger, bool forceStack)
        __attribute__((always_inline));
static void
_markObjectNonNullCommon(const Object *obj, GcMarkContext *ctx,
        bool checkFinger, bool forceStack)
{
    assert(obj != NULL);

#if GC_DEBUG(GC_DEBUG_PARANOID)
//TODO: make sure we're locked
    assert(dvmIsValidObject(obj));
#endif

    if (!setAndReturnMarkBit(ctx, obj)) {
        /* This object was not previously marked.
         */
        if (forceStack || (checkFinger && (void *)obj < ctx->finger)) {
            /* This object will need to go on the mark stack.
             */
            MARK_STACK_PUSH(ctx->stack, obj);
        }

#if WITH_HPROF
        if (gDvm.gcHeap->hprofContext != NULL) {
            hprofMarkRootObject(gDvm.gcHeap->hprofContext, obj, 0);
        }
#endif
#if DVM_TRACK_HEAP_MARKING
        gDvm.gcHeap->markCount++;
        gDvm.gcHeap->markSize += dvmHeapSourceChunkSize((void *)obj) +
                HEAP_SOURCE_CHUNK_OVERHEAD;
#endif

        /* obj->clazz can be NULL if we catch an object between
         * dvmMalloc() and DVM_OBJECT_INIT().  This is ok.
         */
        LOGV_MARK("0x%08x %s\n", (uint)obj,
                obj->clazz == NULL ? "<null class>" : obj->clazz->name);
    }
}

/* Used to mark objects when recursing.  Recursion is done by moving
 * the finger across the bitmaps in address order and marking child
 * objects.  Any newly-marked objects whose addresses are lower than
 * the finger won't be visited by the bitmap scan, so those objects
 * need to be added to the mark stack.
 */
static void
markObjectNonNull(const Object *obj, GcMarkContext *ctx)
{
    _markObjectNonNullCommon(obj, ctx, true, false);
}

#define markObject(obj, ctx) \
    do { \
        Object *MO_obj_ = (Object *)(obj); \
        if (MO_obj_ != NULL) { \
            markObjectNonNull(MO_obj_, (ctx)); \
        } \
    } while (false)

/* If the object hasn't already been marked, mark it and
 * schedule it to be scanned for references.
 *
 * obj may not be NULL.  The macro dvmMarkObject() should
 * be used in situations where a reference may be NULL.
 *
 * This function may only be called when marking the root
 * set.  When recursing, use the internal markObject[NonNull]().
 */
void
dvmMarkObjectNonNull(const Object *obj)
{
    _markObjectNonNullCommon(obj, &gDvm.gcHeap->markContext, false, false);
}

/* Mark the set of root objects.
 *
 * Things we need to scan:
 * - System classes defined by root classloader
 * - For each thread:
 *   - Interpreted stack, from top to "curFrame"
 *     - Dalvik registers (args + local vars)
 *   - JNI local references
 *   - Automatic VM local references (TrackedAlloc)
 *   - Associated Thread/VMThread object
 *   - ThreadGroups (could track & start with these instead of working
 *     upward from Threads)
 *   - Exception currently being thrown, if present
 * - JNI global references
 * - Interned string table
 * - Primitive classes
 * - Special objects
 *   - gDvm.outOfMemoryObj
 * - Objects allocated with ALLOC_NO_GC
 * - Objects pending finalization (but not yet finalized)
 * - Objects in debugger object registry
 *
 * Don't need:
 * - Native stack (for in-progress stuff in the VM)
 *   - The TrackedAlloc stuff watches all native VM references.
 */
void dvmHeapMarkRootSet()
{
    HeapRefTable *refs;
    GcHeap *gcHeap;
    Object **op;

    gcHeap = gDvm.gcHeap;

    HPROF_SET_GC_SCAN_STATE(HPROF_ROOT_STICKY_CLASS, 0);

    LOG_SCAN("root class loader\n");
    dvmGcScanRootClassLoader();
    LOG_SCAN("primitive classes\n");
    dvmGcScanPrimitiveClasses();

    /* dvmGcScanRootThreadGroups() sets a bunch of
     * different scan states internally.
     */
    HPROF_CLEAR_GC_SCAN_STATE();

    LOG_SCAN("root thread groups\n");
    dvmGcScanRootThreadGroups();

    HPROF_SET_GC_SCAN_STATE(HPROF_ROOT_INTERNED_STRING, 0);

    LOG_SCAN("interned strings\n");
    dvmGcScanInternedStrings();

    HPROF_SET_GC_SCAN_STATE(HPROF_ROOT_JNI_GLOBAL, 0);

    LOG_SCAN("JNI global refs\n");
    dvmGcMarkJniGlobalRefs();

    HPROF_SET_GC_SCAN_STATE(HPROF_ROOT_REFERENCE_CLEANUP, 0);

    LOG_SCAN("pending reference operations\n");
    dvmHeapMarkLargeTableRefs(gcHeap->referenceOperations, true);

    HPROF_SET_GC_SCAN_STATE(HPROF_ROOT_FINALIZING, 0);

    LOG_SCAN("pending finalizations\n");
    dvmHeapMarkLargeTableRefs(gcHeap->pendingFinalizationRefs, false);

    HPROF_SET_GC_SCAN_STATE(HPROF_ROOT_DEBUGGER, 0);

    LOG_SCAN("debugger refs\n");
    dvmGcMarkDebuggerRefs();

    HPROF_SET_GC_SCAN_STATE(HPROF_ROOT_VM_INTERNAL, 0);

    /* Mark all ALLOC_NO_GC objects.
     */
    LOG_SCAN("ALLOC_NO_GC objects\n");
    refs = &gcHeap->nonCollectableRefs;
    op = refs->table;
    while ((uintptr_t)op < (uintptr_t)refs->nextEntry) {
        dvmMarkObjectNonNull(*(op++));
    }

    /* Mark any special objects we have sitting around.
     */
    LOG_SCAN("special objects\n");
    dvmMarkObjectNonNull(gDvm.outOfMemoryObj);
    dvmMarkObjectNonNull(gDvm.internalErrorObj);
    dvmMarkObjectNonNull(gDvm.noClassDefFoundErrorObj);
    dvmMarkObject(gDvm.jniWeakGlobalRefQueue);
//TODO: scan object references sitting in gDvm;  use pointer begin & end

    HPROF_CLEAR_GC_SCAN_STATE();
}

/*
 * Nothing past this point is allowed to use dvmMarkObject*().
 * Scanning/recursion must use markObject*(), which takes the
 * finger into account.
 */
#define dvmMarkObjectNonNull __dont_use_dvmMarkObjectNonNull__


/* Mark all of a ClassObject's interfaces.
 */
static void markInterfaces(const ClassObject *clazz, GcMarkContext *ctx)
{
    ClassObject **interfaces;
    int interfaceCount;
    int i;

    /* Mark all interfaces.
     */
    interfaces = clazz->interfaces;
    interfaceCount = clazz->interfaceCount;
    for (i = 0; i < interfaceCount; i++) {
        markObjectNonNull((Object *)*interfaces, ctx);
        interfaces++;
    }
}

/* Mark all objects referred to by a ClassObject's static fields.
 */
static void scanStaticFields(const ClassObject *clazz, GcMarkContext *ctx)
{
    StaticField *f;
    int i;

    //TODO: Optimize this with a bit vector or something
    f = &clazz->sfields[0];
    for (i = 0; i < clazz->sfieldCount; i++) {
        char c = f->field.signature[0];
        if (c == '[' || c == 'L') {
            /* It's an array or class reference.
             */
            markObject((Object *)f->value.l, ctx);
        }
        f++;
    }
}

/* Mark all objects referred to by a DataObject's instance fields.
 */
static void scanInstanceFields(const DataObject *obj, ClassObject *clazz,
        GcMarkContext *ctx)
{
    if (clazz->refOffsets != CLASS_WALK_SUPER) {
        unsigned int refOffsets = clazz->refOffsets;
        while (refOffsets != 0) {
            const int rshift = CLZ(refOffsets);
            refOffsets &= ~(CLASS_HIGH_BIT >> rshift);
            markObject(dvmGetFieldObject((Object*)obj,
                                         CLASS_OFFSET_FROM_CLZ(rshift)), ctx);
        }
    } else {
        while (clazz != NULL) {
            InstField *f;
            int i;

            /* All of the fields that contain object references
             * are guaranteed to be at the beginning of the ifields list.
             */
            f = clazz->ifields;
            for (i = 0; i < clazz->ifieldRefCount; i++) {
                /* Mark the array or object reference.
                 * May be NULL.
                 *
                 * Note that, per the comment on struct InstField,
                 * f->byteOffset is the offset from the beginning of
                 * obj, not the offset into obj->instanceData.
                 */
                markObject(dvmGetFieldObject((Object*)obj, f->byteOffset), ctx);
                f++;
            }

            /* This will be NULL when we hit java.lang.Object
             */
            clazz = clazz->super;
        }
    }
}

/* Mark all objects referred to by the array's contents.
 */
static void scanObjectArray(const ArrayObject *array, GcMarkContext *ctx)
{
    Object **contents;
    u4 length;
    u4 i;

    contents = (Object **)array->contents;
    length = array->length;

    for (i = 0; i < length; i++) {
        markObject(*contents, ctx); // may be NULL
        contents++;
    }
}

/* Mark all objects referred to by the ClassObject.
 */
static void scanClassObject(const ClassObject *clazz, GcMarkContext *ctx)
{
    LOGV_SCAN("---------> %s\n", clazz->name);

    if (IS_CLASS_FLAG_SET(clazz, CLASS_ISARRAY)) {
        /* We're an array; mark the class object of the contents
         * of the array.
         *
         * Note that we won't necessarily reach the array's element
         * class by scanning the array contents;  the array may be
         * zero-length, or may only contain null objects.
         */
        markObjectNonNull((Object *)clazz->elementClass, ctx);
    }

    /* We scan these explicitly in case the only remaining
     * reference to a particular class object is via a data
     * object;  we may not be guaranteed to reach all
     * live class objects via a classloader.
     */
    markObject((Object *)clazz->super, ctx);  // may be NULL (java.lang.Object)
    markObject(clazz->classLoader, ctx);      // may be NULL

    scanStaticFields(clazz, ctx);
    markInterfaces(clazz, ctx);
}

/* Mark all objects that obj refers to.
 *
 * Called on every object in markList.
 */
static void scanObject(const Object *obj, GcMarkContext *ctx)
{
    ClassObject *clazz;

    assert(dvmIsValidObject(obj));
    LOGV_SCAN("0x%08x %s\n", (uint)obj, obj->clazz->descriptor);

#if WITH_HPROF
    if (gDvm.gcHeap->hprofContext != NULL) {
        hprofDumpHeapObject(gDvm.gcHeap->hprofContext, obj);
    }
#endif

    /* Get and mark the class object for this particular instance.
     */
    clazz = obj->clazz;
    if (clazz == NULL) {
        /* This can happen if we catch an object between dvmMalloc()
         * and DVM_OBJECT_INIT().  The object won't contain any
         * references yet, so we can just skip it.  It can also mean
         * that this object is unlinkedJavaLangClass, which has its
         * clazz explicitly set to NULL.
         */
        return;
    } else if (clazz == gDvm.unlinkedJavaLangClass) {
        /* This class hasn't been linked yet.  We're guaranteed
         * that the object doesn't contain any references that
         * aren't already tracked, so we can skip scanning it.
         */
        return;
    }

    assert(dvmIsValidObject((Object *)clazz));
    markObjectNonNull((Object *)clazz, ctx);

    /* Mark any references in this object.
     */
    if (IS_CLASS_FLAG_SET(clazz, CLASS_ISARRAY)) {
        /* It's an array object.
         */
        if (IS_CLASS_FLAG_SET(clazz, CLASS_ISOBJECTARRAY)) {
            /* It's an array of object references.
             */
            scanObjectArray((ArrayObject *)obj, ctx);
        }
        // else there's nothing else to scan
    } else {
        /* It's a DataObject-compatible object.
         */
        scanInstanceFields((DataObject *)obj, clazz, ctx);

        if (IS_CLASS_FLAG_SET(clazz, CLASS_ISREFERENCE)) {
            GcHeap *gcHeap = gDvm.gcHeap;
            Object *referent;

            /* It's a subclass of java/lang/ref/Reference.
             * The fields in this class have been arranged
             * such that scanInstanceFields() did not actually
             * mark the "referent" field;  we need to handle
             * it specially.
             *
             * If the referent already has a strong mark (isMarked(referent)),
             * we don't care about its reference status.
             */
            referent = dvmGetFieldObject(obj,
                    gDvm.offJavaLangRefReference_referent);
            if (referent != NULL &&
                    !isMarked(referent, &gcHeap->markContext))
            {
                u4 refFlags;

                if (gcHeap->markAllReferents) {
                    LOG_REF("Hard-marking a reference\n");

                    /* Don't bother with normal reference-following
                     * behavior, just mark the referent.  This should
                     * only be used when following objects that just
                     * became scheduled for finalization.
                     */
                    markObjectNonNull(referent, ctx);
                    goto skip_reference;
                }

                /* See if this reference was handled by a previous GC.
                 */
                if (dvmGetFieldObject(obj,
                            gDvm.offJavaLangRefReference_vmData) ==
                        SCHEDULED_REFERENCE_MAGIC)
                {
                    LOG_REF("Skipping scheduled reference\n");

                    /* Don't reschedule it, but make sure that its
                     * referent doesn't get collected (in case it's
                     * a PhantomReference and wasn't cleared automatically).
                     */
                    //TODO: Mark these after handling all new refs of
                    //      this strength, in case the new refs refer
                    //      to the same referent.  Not a very common
                    //      case, though.
                    markObjectNonNull(referent, ctx);
                    goto skip_reference;
                }

                /* Find out what kind of reference is pointing
                 * to referent.
                 */
                refFlags = GET_CLASS_FLAG_GROUP(clazz,
                    CLASS_ISREFERENCE |
                    CLASS_ISWEAKREFERENCE |
                    CLASS_ISPHANTOMREFERENCE);

            /* We use the vmData field of Reference objects
             * as a next pointer in a singly-linked list.
             * That way, we don't need to allocate any memory
             * while we're doing a GC.
             */
#define ADD_REF_TO_LIST(list, ref) \
            do { \
                Object *ARTL_ref_ = (/*de-const*/Object *)(ref); \
                dvmSetFieldObject(ARTL_ref_, \
                        gDvm.offJavaLangRefReference_vmData, list); \
                list = ARTL_ref_; \
            } while (false)

                /* At this stage, we just keep track of all of
                 * the live references that we've seen.  Later,
                 * we'll walk through each of these lists and
                 * deal with the referents.
                 */
                if (refFlags == CLASS_ISREFERENCE) {
                    /* It's a soft reference.  Depending on the state,
                     * we'll attempt to collect all of them, some of
                     * them, or none of them.
                     */
                    if (gcHeap->softReferenceCollectionState ==
                            SR_COLLECT_NONE)
                    {
                sr_collect_none:
                        markObjectNonNull(referent, ctx);
                    } else if (gcHeap->softReferenceCollectionState ==
                            SR_COLLECT_ALL)
                    {
                sr_collect_all:
                        ADD_REF_TO_LIST(gcHeap->softReferences, obj);
                    } else {
                        /* We'll only try to collect half of the
                         * referents.
                         */
                        if (gcHeap->softReferenceColor++ & 1) {
                            goto sr_collect_none;
                        }
                        goto sr_collect_all;
                    }
                } else {
                    /* It's a weak or phantom reference.
                     * Clearing CLASS_ISREFERENCE will reveal which.
                     */
                    refFlags &= ~CLASS_ISREFERENCE;
                    if (refFlags == CLASS_ISWEAKREFERENCE) {
                        ADD_REF_TO_LIST(gcHeap->weakReferences, obj);
                    } else if (refFlags == CLASS_ISPHANTOMREFERENCE) {
                        ADD_REF_TO_LIST(gcHeap->phantomReferences, obj);
                    } else {
                        assert(!"Unknown reference type");
                    }
                }
#undef ADD_REF_TO_LIST
            }
        }

    skip_reference:
        /* If this is a class object, mark various other things that
         * its internals point to.
         *
         * All class objects are instances of java.lang.Class,
         * including the java.lang.Class class object.
         */
        if (clazz == gDvm.classJavaLangClass) {
            scanClassObject((ClassObject *)obj, ctx);
        }
    }
}

static void
processMarkStack(GcMarkContext *ctx)
{
    const Object **const base = ctx->stack.base;

    /* Scan anything that's on the mark stack.
     * We can't use the bitmaps anymore, so use
     * a finger that points past the end of them.
     */
    ctx->finger = (void *)ULONG_MAX;
    while (ctx->stack.top != base) {
        scanObject(*ctx->stack.top++, ctx);
    }
}

#ifndef NDEBUG
static uintptr_t gLastFinger = 0;
#endif

static bool
scanBitmapCallback(size_t numPtrs, void **ptrs, const void *finger, void *arg)
{
    GcMarkContext *ctx = (GcMarkContext *)arg;
    size_t i;

#ifndef NDEBUG
    assert((uintptr_t)finger >= gLastFinger);
    gLastFinger = (uintptr_t)finger;
#endif

    ctx->finger = finger;
    for (i = 0; i < numPtrs; i++) {
        scanObject(*ptrs++, ctx);
    }

    return true;
}

/* Given bitmaps with the root set marked, find and mark all
 * reachable objects.  When this returns, the entire set of
 * live objects will be marked and the mark stack will be empty.
 */
void dvmHeapScanMarkedObjects()
{
    GcMarkContext *ctx = &gDvm.gcHeap->markContext;

    assert(ctx->finger == NULL);

    /* The bitmaps currently have bits set for the root set.
     * Walk across the bitmaps and scan each object.
     */
#ifndef NDEBUG
    gLastFinger = 0;
#endif
    dvmHeapBitmapWalk(ctx->bitmap, scanBitmapCallback, ctx);

    /* We've walked the mark bitmaps.  Scan anything that's
     * left on the mark stack.
     */
    processMarkStack(ctx);

    LOG_SCAN("done with marked objects\n");
}

/** Clear the referent field.
 */
static void clearReference(Object *reference)
{
    /* This is what the default implementation of Reference.clear()
     * does.  We're required to clear all references to a given
     * referent atomically, so we can't pop in and out of interp
     * code each time.
     *
     * We don't ever actaully call overriding implementations of
     * Reference.clear().
     */
    dvmSetFieldObject(reference,
            gDvm.offJavaLangRefReference_referent, NULL);
}

/** @return true if we need to schedule a call to enqueue().
 */
static bool enqueueReference(Object *reference)
{
    Object *queue = dvmGetFieldObject(reference,
            gDvm.offJavaLangRefReference_queue);
    Object *queueNext = dvmGetFieldObject(reference,
            gDvm.offJavaLangRefReference_queueNext);
    if (queue == NULL || queueNext != NULL) {
        /* There is no queue, or the reference has already
         * been enqueued.  The Reference.enqueue() method
         * will do nothing even if we call it.
         */
        return false;
    }

    /* We need to call enqueue(), but if we called it from
     * here we'd probably deadlock.  Schedule a call.
     */
    return true;
}

/* All objects for stronger reference levels have been
 * marked before this is called.
 */
void dvmHeapHandleReferences(Object *refListHead, enum RefType refType)
{
    Object *reference;
    GcMarkContext *markContext = &gDvm.gcHeap->markContext;
    const int offVmData = gDvm.offJavaLangRefReference_vmData;
    const int offReferent = gDvm.offJavaLangRefReference_referent;
    bool workRequired = false;

    reference = refListHead;
    while (reference != NULL) {
        Object *next;
        Object *referent;

        /* Pull the interesting fields out of the Reference object.
         */
        next = dvmGetFieldObject(reference, offVmData);
        referent = dvmGetFieldObject(reference, offReferent);

        //TODO: when handling REF_PHANTOM, unlink any references
        //      that fail this initial if().  We need to re-walk
        //      the list, and it would be nice to avoid the extra
        //      work.
        if (referent != NULL && !isMarked(referent, markContext)) {
            bool schedEnqueue;

            /* This is the strongest reference that refers to referent.
             * Do the right thing.
             */
            switch (refType) {
            case REF_SOFT:
            case REF_WEAK:
                clearReference(reference);
                schedEnqueue = enqueueReference(reference);
                break;
            case REF_PHANTOM:
                /* PhantomReferences are not cleared automatically.
                 * Until someone clears it (or the reference itself
                 * is collected), the referent must remain alive.
                 *
                 * It's necessary to fully mark the referent because
                 * it will still be present during the next GC, and
                 * all objects that it points to must be valid.
                 * (The referent will be marked outside of this loop,
                 * after handing all references of this strength, in
                 * case multiple references point to the same object.)
                 *
                 * One exception: JNI "weak global" references are handled
                 * as a special case.  They're identified by the queue.
                 */
                if (gDvm.jniWeakGlobalRefQueue != NULL) {
                    Object* queue = dvmGetFieldObject(reference,
                            gDvm.offJavaLangRefReference_queue);
                    if (queue == gDvm.jniWeakGlobalRefQueue) {
                        LOGV("+++ WGR: clearing + not queueing %p:%p\n",
                            reference, referent);
                        clearReference(reference);
                        schedEnqueue = false;
                        break;
                    }
                }

                /* A PhantomReference is only useful with a
                 * queue, but since it's possible to create one
                 * without a queue, we need to check.
                 */
                schedEnqueue = enqueueReference(reference);
                break;
            default:
                assert(!"Bad reference type");
                schedEnqueue = false;
                break;
            }

            if (schedEnqueue) {
                /* Stuff the enqueue bit in the bottom of the pointer.
                 * Assumes that objects are 8-byte aligned.
                 *
                 * Note that we are adding the *Reference* (which
                 * is by definition already marked at this point) to
                 * this list; we're not adding the referent (which
                 * has already been cleared).
                 */
                assert(((intptr_t)reference & 3) == 0);
                assert((WORKER_ENQUEUE & ~3) == 0);
                if (!dvmHeapAddRefToLargeTable(
                        &gDvm.gcHeap->referenceOperations,
                        (Object *)((uintptr_t)reference | WORKER_ENQUEUE)))
                {
                    LOGE_HEAP("dvmMalloc(): no room for any more "
                            "reference operations\n");
                    dvmAbort();
                }
                workRequired = true;
            }

            if (refType != REF_PHANTOM) {
                /* Let later GCs know not to reschedule this reference.
                 */
                dvmSetFieldObject(reference, offVmData,
                        SCHEDULED_REFERENCE_MAGIC);
            } // else this is handled later for REF_PHANTOM

        } // else there was a stronger reference to the referent.

        reference = next;
    }

    /* Walk though the reference list again, and mark any non-clear/marked
     * referents.  Only PhantomReferences can have non-clear referents
     * at this point.
     *
     * (Could skip this for JNI weak globals, since we know they've been
     * cleared.)
     */
    if (refType == REF_PHANTOM) {
        bool scanRequired = false;

        HPROF_SET_GC_SCAN_STATE(HPROF_ROOT_REFERENCE_CLEANUP, 0);
        reference = refListHead;
        while (reference != NULL) {
            Object *next;
            Object *referent;

            /* Pull the interesting fields out of the Reference object.
             */
            next = dvmGetFieldObject(reference, offVmData);
            referent = dvmGetFieldObject(reference, offReferent);

            if (referent != NULL && !isMarked(referent, markContext)) {
                markObjectNonNull(referent, markContext);
                scanRequired = true;

                /* Let later GCs know not to reschedule this reference.
                 */
                dvmSetFieldObject(reference, offVmData,
                        SCHEDULED_REFERENCE_MAGIC);
            }

            reference = next;
        }
        HPROF_CLEAR_GC_SCAN_STATE();

        if (scanRequired) {
            processMarkStack(markContext);
        }
    }

    if (workRequired) {
        dvmSignalHeapWorker(false);
    }
}


/* Find unreachable objects that need to be finalized,
 * and schedule them for finalization.
 */
void dvmHeapScheduleFinalizations()
{
    HeapRefTable newPendingRefs;
    LargeHeapRefTable *finRefs = gDvm.gcHeap->finalizableRefs;
    Object **ref;
    Object **lastRef;
    size_t totalPendCount;
    GcMarkContext *markContext = &gDvm.gcHeap->markContext;

    /*
     * All reachable objects have been marked.
     * Any unmarked finalizable objects need to be finalized.
     */

    /* Create a table that the new pending refs will
     * be added to.
     */
    if (!dvmHeapInitHeapRefTable(&newPendingRefs, 128)) {
        //TODO: mark all finalizable refs and hope that
        //      we can schedule them next time.  Watch out,
        //      because we may be expecting to free up space
        //      by calling finalizers.
        LOGE_GC("dvmHeapScheduleFinalizations(): no room for "
                "pending finalizations\n");
        dvmAbort();
    }

    /* Walk through finalizableRefs and move any unmarked references
     * to the list of new pending refs.
     */
    totalPendCount = 0;
    while (finRefs != NULL) {
        Object **gapRef;
        size_t newPendCount = 0;

        gapRef = ref = finRefs->refs.table;
        lastRef = finRefs->refs.nextEntry;
        while (ref < lastRef) {
            if (!isMarked(*ref, markContext)) {
                if (!dvmHeapAddToHeapRefTable(&newPendingRefs, *ref)) {
                    //TODO: add the current table and allocate
                    //      a new, smaller one.
                    LOGE_GC("dvmHeapScheduleFinalizations(): "
                            "no room for any more pending finalizations: %zd\n",
                            dvmHeapNumHeapRefTableEntries(&newPendingRefs));
                    dvmAbort();
                }
                newPendCount++;
            } else {
                /* This ref is marked, so will remain on finalizableRefs.
                 */
                if (newPendCount > 0) {
                    /* Copy it up to fill the holes.
                     */
                    *gapRef++ = *ref;
                } else {
                    /* No holes yet; don't bother copying.
                     */
                    gapRef++;
                }
            }
            ref++;
        }
        finRefs->refs.nextEntry = gapRef;
        //TODO: if the table is empty when we're done, free it.
        totalPendCount += newPendCount;
        finRefs = finRefs->next;
    }
    LOGD_GC("dvmHeapScheduleFinalizations(): %zd finalizers triggered.\n",
            totalPendCount);
    if (totalPendCount == 0) {
        /* No objects required finalization.
         * Free the empty temporary table.
         */
        dvmClearReferenceTable(&newPendingRefs);
        return;
    }

    /* Add the new pending refs to the main list.
     */
    if (!dvmHeapAddTableToLargeTable(&gDvm.gcHeap->pendingFinalizationRefs,
                &newPendingRefs))
    {
        LOGE_GC("dvmHeapScheduleFinalizations(): can't insert new "
                "pending finalizations\n");
        dvmAbort();
    }

    //TODO: try compacting the main list with a memcpy loop

    /* Mark the refs we just moved;  we don't want them or their
     * children to get swept yet.
     */
    ref = newPendingRefs.table;
    lastRef = newPendingRefs.nextEntry;
    assert(ref < lastRef);
    HPROF_SET_GC_SCAN_STATE(HPROF_ROOT_FINALIZING, 0);
    while (ref < lastRef) {
        markObjectNonNull(*ref, markContext);
        ref++;
    }
    HPROF_CLEAR_GC_SCAN_STATE();

    /* Set markAllReferents so that we don't collect referents whose
     * only references are in final-reachable objects.
     * TODO: eventually provide normal reference behavior by properly
     *       marking these references.
     */
    gDvm.gcHeap->markAllReferents = true;
    processMarkStack(markContext);
    gDvm.gcHeap->markAllReferents = false;

    dvmSignalHeapWorker(false);
}

void dvmHeapFinishMarkStep()
{
    GcMarkContext *markContext;

    markContext = &gDvm.gcHeap->markContext;

    /* The sweep step freed every object that appeared in the
     * HeapSource bitmaps that didn't appear in the mark bitmaps.
     * The new state of the HeapSource is exactly the final
     * mark bitmaps, so swap them in.
     */
    dvmHeapSourceSwapBitmaps();

    /* Clean up everything else associated with the marking process.
     */
    destroyMarkStack(&markContext->stack);

    markContext->finger = NULL;
}

static bool
sweepBitmapCallback(size_t numPtrs, void **ptrs, const void *finger, void *arg)
{
    const ClassObject *const classJavaLangClass = gDvm.classJavaLangClass;
    const bool overwriteFree = gDvm.overwriteFree;
    size_t i;
    void **origPtrs = ptrs;

    for (i = 0; i < numPtrs; i++) {
        Object *obj;

        obj = (Object *)*ptrs++;

        /* NOTE: Dereferencing clazz is dangerous.  If obj was the last
         * one to reference its class object, the class object could be
         * on the sweep list, and could already have been swept, leaving
         * us with a stale pointer.
         */
        LOGV_SWEEP("FREE: 0x%08x %s\n", (uint)obj, obj->clazz->name);

        /* This assumes that java.lang.Class will never go away.
         * If it can, and we were the last reference to it, it
         * could have already been swept.  However, even in that case,
         * gDvm.classJavaLangClass should still have a useful
         * value.
         */
        if (obj->clazz == classJavaLangClass) {
            LOGV_SWEEP("---------------> %s\n", ((ClassObject *)obj)->name);
            /* dvmFreeClassInnards() may have already been called,
             * but it's safe to call on the same ClassObject twice.
             */
            dvmFreeClassInnards((ClassObject *)obj);
        }

        /* Overwrite the to-be-freed object to make stale references
         * more obvious.
         */
        if (overwriteFree) {
            int objlen;
            ClassObject *clazz = obj->clazz;
            objlen = dvmHeapSourceChunkSize(obj);
            memset(obj, 0xa5, objlen);
            obj->clazz = (ClassObject *)((uintptr_t)clazz ^ 0xffffffff);
        }
    }
    // TODO: dvmHeapSourceFreeList has a loop, just like the above
    // does. Consider collapsing the two loops to save overhead.
    dvmHeapSourceFreeList(numPtrs, origPtrs);

    return true;
}

/* Returns true if the given object is unmarked.  Ignores the low bits
 * of the pointer because the intern table may set them.
 */
static int isUnmarkedObject(void *object)
{
    return !isMarked((void *)((uintptr_t)object & ~(HB_OBJECT_ALIGNMENT-1)),
            &gDvm.gcHeap->markContext);
}

/* Walk through the list of objects that haven't been
 * marked and free them.
 */
void
dvmHeapSweepUnmarkedObjects(int *numFreed, size_t *sizeFreed)
{
    HeapBitmap markBits[HEAP_SOURCE_MAX_HEAP_COUNT];
    HeapBitmap objBits[HEAP_SOURCE_MAX_HEAP_COUNT];
    size_t origObjectsAllocated;
    size_t origBytesAllocated;
    size_t numBitmaps;

    /* All reachable objects have been marked.
     * Detach any unreachable interned strings before
     * we sweep.
     */
    dvmGcDetachDeadInternedStrings(isUnmarkedObject);

    /* Free any known objects that are not marked.
     */
    origObjectsAllocated = dvmHeapSourceGetValue(HS_OBJECTS_ALLOCATED, NULL, 0);
    origBytesAllocated = dvmHeapSourceGetValue(HS_BYTES_ALLOCATED, NULL, 0);

    dvmSweepMonitorList(&gDvm.monitorList, isUnmarkedObject);

    numBitmaps = dvmHeapSourceGetNumHeaps();
    dvmHeapSourceGetObjectBitmaps(objBits, markBits, numBitmaps);
    dvmHeapBitmapXorWalkLists(markBits, objBits, numBitmaps,
                              sweepBitmapCallback, NULL);

    *numFreed = origObjectsAllocated -
            dvmHeapSourceGetValue(HS_OBJECTS_ALLOCATED, NULL, 0);
    *sizeFreed = origBytesAllocated -
            dvmHeapSourceGetValue(HS_BYTES_ALLOCATED, NULL, 0);

#ifdef WITH_PROFILER
    if (gDvm.allocProf.enabled) {
        gDvm.allocProf.freeCount += *numFreed;
        gDvm.allocProf.freeSize += *sizeFreed;
    }
#endif
}
