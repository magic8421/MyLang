#include "runtime.h"

/* Thread-local stack trace state */
MY_TL const char* __my_file = NULL;
MY_TL int         __my_line = 0;
MY_TL MyFrame     __my_stack[MY_STACK_MAX];
MY_TL int         __my_depth = 0;

void my_backtrace(void) {
    if (__my_depth == 0) {
        fprintf(stderr, "  (stack empty)\n");
        return;
    }
    fprintf(stderr, "Backtrace (most recent call first):\n");
    {
        int _i;
        for (_i = __my_depth - 1; _i >= 0; _i--) {
            MyFrame* _fr = &__my_stack[_i];
            fprintf(stderr, "  #%d %s at %s:%d\n", __my_depth - 1 - _i,
                    _fr->func ? _fr->func : "???",
                    _fr->file ? _fr->file : "???",
                    _fr->line);
        }
    }
}

void my_panic(const char* msg) {
    fprintf(stderr, "Panic: %s\n", msg);
    fprintf(stderr, "  -> triggered at %s:%d\n",
            __my_file ? __my_file : "?", __my_line);
    my_backtrace();
    abort();
}

/* Core allocation for class/interface objects. */
void* mylang_new_object(size_t sz, uint32_t type_id, void (*dtor)(void*)) {
    ObjHeader* h = (ObjHeader*)calloc(1, sizeof(ObjHeader) + sz);
    h->refcount = 1;
    h->type_id = type_id;
    h->dtor = dtor;
    mylang_leak_insert(h);
    return (void*)(h + 1);
}

void* mylang_retain(void* ptr) {
    if (ptr) mylang_atomic_inc(&mylang_obj_hdr(ptr)->refcount);
    return ptr;
}

/* Generic interface pointer layout used by release. */
typedef struct { void* data; const void* vtable; } AnyInterface;

int mylang_release(void* ptr) {
    if (ptr && mylang_atomic_dec(&mylang_obj_hdr(ptr)->refcount) == 0) {
        ObjHeader* h = mylang_obj_hdr(ptr);
        if (h->weak) h->weak->obj = NULL;
        if (h->dtor) h->dtor(ptr);
        mylang_leak_remove(h);
        free(h);
    }
    return 0;
}

/* --- Array helpers ------------------------------------------------------ */

static void mylang_array_release_elements(void* data, size_t length, int elem_kind) {
    size_t i;
    switch (elem_kind) {
        case MYLANG_ELEM_CLASS: {
            void** p = (void**)data;
            for (i = 0; i < length; i++) mylang_release(p[i]);
            break;
        }
        case MYLANG_ELEM_INTERFACE: {
            AnyInterface* p = (AnyInterface*)data;
            for (i = 0; i < length; i++) mylang_release(p[i].data);
            break;
        }
        case MYLANG_ELEM_WEAK_CLASS: {
            WeakRef** p = (WeakRef**)data;
            for (i = 0; i < length; i++) mylang_weak_release(p[i]);
            break;
        }
        case MYLANG_ELEM_WEAK_IFACE: {
            typedef struct { WeakRef* wr; const void* vt; } AnyWeakInterface;
            AnyWeakInterface* p = (AnyWeakInterface*)data;
            for (i = 0; i < length; i++) mylang_weak_release(p[i].wr);
            break;
        }
        case MYLANG_ELEM_PRIMITIVE:
        case MYLANG_ELEM_STRUCT:
        default:
            break;
    }
}

void mylang_array_init(MyArray* a, size_t count, size_t elem_size) {
    a->capacity = count;
    a->length = count;
    if (count == 0 || elem_size == 0) {
        a->data = NULL;
    } else {
        a->data = calloc(count, elem_size);
    }
}

void mylang_array_free(MyArray* a, size_t elem_size, int elem_kind) {
    (void)elem_size;
    if (a->data) {
        mylang_array_release_elements(a->data, a->length, elem_kind);
        free(a->data);
    }
    a->capacity = 0;
    a->length = 0;
    a->data = NULL;
}

void mylang_array_reserve(MyArray* a, size_t new_capacity, size_t elem_size) {
    if (new_capacity <= a->capacity) return;
    if (new_capacity < a->capacity * 2) new_capacity = a->capacity * 2;
    if (new_capacity < 4) new_capacity = 4;
    a->data = realloc(a->data, new_capacity * elem_size);
    if (!a->data) my_panic("array reserve failed");
    a->capacity = new_capacity;
}

void mylang_array_resize(MyArray* a, size_t new_length, size_t elem_size, int elem_kind) {
    if (new_length > a->capacity) {
        mylang_array_reserve(a, new_length, elem_size);
    }
    if (new_length < a->length) {
        /* Release dropped elements. */
        char* p = (char*)a->data + new_length * elem_size;
        mylang_array_release_elements(p, a->length - new_length, elem_kind);
    }
    if (new_length > a->length) {
        /* Zero new slots. */
        char* p = (char*)a->data + a->length * elem_size;
        memset(p, 0, (new_length - a->length) * elem_size);
    }
    a->length = new_length;
}

void mylang_array_move(MyArray* src, MyArray* dst, size_t elem_size, int elem_kind) {
    mylang_array_free(dst, elem_size, elem_kind);
    *dst = *src;
    src->capacity = 0;
    src->length = 0;
    src->data = NULL;
}

void mylang_array_copy(const MyArray* src, MyArray* dst, size_t elem_size, int elem_kind) {
    size_t i;
    mylang_array_free(dst, elem_size, elem_kind);
    if (src->length == 0) {
        mylang_array_init(dst, 0, elem_size);
        return;
    }
    mylang_array_init(dst, src->length, elem_size);
    memcpy(dst->data, src->data, src->length * elem_size);
    /* Retain reference-type elements. */
    switch (elem_kind) {
        case MYLANG_ELEM_CLASS: {
            void** p = (void**)dst->data;
            for (i = 0; i < dst->length; i++) mylang_retain(p[i]);
            break;
        }
        case MYLANG_ELEM_INTERFACE: {
            AnyInterface* p = (AnyInterface*)dst->data;
            for (i = 0; i < dst->length; i++) mylang_retain(p[i].data);
            break;
        }
        case MYLANG_ELEM_WEAK_CLASS: {
            WeakRef** p = (WeakRef**)dst->data;
            for (i = 0; i < dst->length; i++) mylang_weak_copy(p[i]);
            break;
        }
        case MYLANG_ELEM_WEAK_IFACE: {
            typedef struct { WeakRef* wr; const void* vt; } AnyWeakInterface;
            AnyWeakInterface* p = (AnyWeakInterface*)dst->data;
            for (i = 0; i < dst->length; i++) p[i].wr = mylang_weak_copy(p[i].wr);
            break;
        }
        default:
            break;
    }
}

void* mylang_array_at(MyArray* a, size_t idx, size_t elem_size) {
    MY_CHECK(idx < a->length, "index out of bounds");
    return (char*)a->data + idx * elem_size;
}

void mylang_array_push(MyArray* a, size_t elem_size, int elem_kind, const void* value) {
    if (a->length == a->capacity) {
        size_t nc = a->capacity ? a->capacity * 2 : 4;
        mylang_array_reserve(a, nc, elem_size);
    }
    {
        char* slot = (char*)a->data + a->length * elem_size;
        switch (elem_kind) {
            case MYLANG_ELEM_CLASS:
                *(void**)slot = mylang_retain(*(void**)value);
                break;
            case MYLANG_ELEM_INTERFACE:
                mylang_release(((AnyInterface*)slot)->data);
                memcpy(slot, value, elem_size);
                mylang_retain(((AnyInterface*)slot)->data);
                break;
            case MYLANG_ELEM_WEAK_CLASS:
                mylang_weak_release(*(WeakRef**)slot);
                memcpy(slot, value, elem_size);
                break;
            case MYLANG_ELEM_WEAK_IFACE: {
                typedef struct { WeakRef* wr; const void* vt; } AnyWeakInterface;
                mylang_weak_release(((AnyWeakInterface*)slot)->wr);
                memcpy(slot, value, elem_size);
                break;
            }
            case MYLANG_ELEM_PRIMITIVE:
            case MYLANG_ELEM_STRUCT:
            default:
                memcpy(slot, value, elem_size);
                break;
        }
    }
    a->length++;
}

void mylang_array_pop(MyArray* a, size_t elem_size, int elem_kind) {
    if (a->length == 0) return;
    a->length--;
    mylang_array_release_elements((char*)a->data + a->length * elem_size, 1, elem_kind);
}

void mylang_array_clear(MyArray* a, size_t elem_size, int elem_kind) {
    (void)elem_size;
    mylang_array_release_elements(a->data, a->length, elem_kind);
    a->length = 0;
}

void mylang_array_compact(MyArray* a, size_t elem_size) {
    if (a->length == a->capacity) return;
    if (a->length == 0) {
        free(a->data);
        a->data = NULL;
        a->capacity = 0;
        return;
    }
    a->data = realloc(a->data, a->length * elem_size);
    if (!a->data) my_panic("array compact failed");
    a->capacity = a->length;
}

/* --- Weak references ---------------------------------------------------- */

WeakRef* mylang_weak_init(void* ptr) {
    ObjHeader* h = mylang_obj_hdr(ptr);
    if (h->weak) {
        mylang_atomic_inc(&h->weak->refcount);
        return h->weak;
    }
    WeakRef* wr = (WeakRef*)calloc(1, sizeof(WeakRef));
    wr->refcount = 1;
    wr->obj = h;
    h->weak = wr;
    return wr;
}

WeakRef* mylang_weak_copy(WeakRef* wr) {
    if (wr) mylang_atomic_inc(&wr->refcount);
    return wr;
}

void* mylang_lock(WeakRef* wr) {
    if (!wr) return NULL;
    ObjHeader* h = wr->obj;
    if (!h) return NULL;
    for (;;) {
        long old = h->refcount;
        if (old <= 0) return NULL;
        if (mylang_atomic_cas(&h->refcount, old + 1, old) == old) {
            return (void*)(h + 1);
        }
    }
}

void mylang_weak_release(WeakRef* wr) {
    if (wr && mylang_atomic_dec(&wr->refcount) == 0) {
        if (wr->obj) wr->obj->weak = NULL;
        free(wr);
    }
}

/* --- Leak checking ------------------------------------------------------ */

#ifdef MYLANG_LEAK_CHECK

#ifdef _MSC_VER
static SRWLOCK mylang_leak_lock = SRWLOCK_INIT;
#define MYLANG_LEAK_LOCK() AcquireSRWLockExclusive(&mylang_leak_lock)
#define MYLANG_LEAK_UNLOCK() ReleaseSRWLockExclusive(&mylang_leak_lock)
#else
#include <pthread.h>
static pthread_mutex_t mylang_leak_mutex = PTHREAD_MUTEX_INITIALIZER;
#define MYLANG_LEAK_LOCK() pthread_mutex_lock(&mylang_leak_mutex)
#define MYLANG_LEAK_UNLOCK() pthread_mutex_unlock(&mylang_leak_mutex)
#endif

static int mylang_leak_atexit_done = 0;

#define LEAK_HASH_SIZE 512

typedef struct LeakTraceTag {
    uint32_t hash;
    int depth;
    struct LeakTraceTag* next;
    MyFrame frames[1];
} LeakTrace;

static LeakTrace* mylang_leak_traces[LEAK_HASH_SIZE];

static uint32_t mylang_leak_hash_frames(const MyFrame* frames, int depth) {
    uint32_t h = 5381;
    int i;
    for (i = 0; i < depth; i++) {
        const char* s;
        s = frames[i].func ? frames[i].func : "";
        while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
        s = frames[i].file ? frames[i].file : "";
        while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
        h = ((h << 5) + h) + (unsigned)(frames[i].line);
    }
    return h;
}

static LeakTrace* mylang_leak_capture(void) {
    int depth = __my_depth;
    int i;
    uint32_t h;
    uint32_t bucket;
    LeakTrace* t;
    if (depth > MY_STACK_MAX) depth = MY_STACK_MAX;
    if (depth < 0) depth = 0;
    h = mylang_leak_hash_frames(__my_stack, depth);
    bucket = h % LEAK_HASH_SIZE;
    t = mylang_leak_traces[bucket];
    while (t) {
        if (t->hash == h && t->depth == depth) {
            int match = 1;
            for (i = 0; i < depth; i++) {
                if (t->frames[i].func != __my_stack[i].func ||
                    t->frames[i].file != __my_stack[i].file ||
                    t->frames[i].line != __my_stack[i].line) {
                    match = 0;
                    break;
                }
            }
            if (match) return t;
        }
        t = t->next;
    }
    t = (LeakTrace*)calloc(1, sizeof(LeakTrace) + (depth > 0 ? depth - 1 : 0) * sizeof(MyFrame));
    if (!t) return NULL;
    t->hash = h;
    t->depth = depth;
    for (i = 0; i < depth; i++) t->frames[i] = __my_stack[i];
    t->next = mylang_leak_traces[bucket];
    mylang_leak_traces[bucket] = t;
    return t;
}

static ObjHeader* mylang_leak_head = NULL;

static void mylang_leak_check(void);

void mylang_leak_insert(ObjHeader* h) {
    MYLANG_LEAK_LOCK();
    if (!mylang_leak_atexit_done) {
        atexit(mylang_leak_check);
        mylang_leak_atexit_done = 1;
    }
    h->alloc_trace = mylang_leak_capture();
    if (mylang_leak_head) {
        h->next = mylang_leak_head;
        h->prev = mylang_leak_head->prev;
        mylang_leak_head->prev->next = h;
        mylang_leak_head->prev = h;
    } else {
        mylang_leak_head = h;
        h->next = h;
        h->prev = h;
    }
    MYLANG_LEAK_UNLOCK();
}

void mylang_leak_remove(ObjHeader* h) {
    MYLANG_LEAK_LOCK();
    if (h->next == h) {
        mylang_leak_head = NULL;
    } else {
        if (mylang_leak_head == h) mylang_leak_head = h->next;
        h->prev->next = h->next;
        h->next->prev = h->prev;
    }
    h->next = NULL;
    h->prev = NULL;
    MYLANG_LEAK_UNLOCK();
}

static void mylang_leak_check(void) {
    MYLANG_LEAK_LOCK();
    if (mylang_leak_head) {
        int count = 0;
        ObjHeader* h = mylang_leak_head;
        fprintf(stderr, "\n========== Memory Leak Report ==========\n");
        do {
            count++;
            fprintf(stderr, "LEAK[%d]: ptr=%p type_id=0x%08X refcount=%ld\n",
                    count, (void*)(h + 1), (unsigned)h->type_id, (long)h->refcount);
            if (h->alloc_trace) {
                LeakTrace* tr = h->alloc_trace;
                int i;
                for (i = tr->depth - 1; i >= 0; i--) {
                    fprintf(stderr, "  #%d %s at %s:%d\n",
                            tr->depth - 1 - i,
                            tr->frames[i].func ? tr->frames[i].func : "???",
                            tr->frames[i].file ? tr->frames[i].file : "???",
                            tr->frames[i].line);
                }
            }
            h = h->next;
        } while (h != mylang_leak_head);
        fprintf(stderr, "Total leaked blocks: %d\n", count);
    }
    {
        int b;
        for (b = 0; b < LEAK_HASH_SIZE; b++) {
            LeakTrace* t = mylang_leak_traces[b];
            while (t) {
                LeakTrace* next = t->next;
                free(t);
                t = next;
            }
        }
    }
    MYLANG_LEAK_UNLOCK();
}

#endif /* MYLANG_LEAK_CHECK */
