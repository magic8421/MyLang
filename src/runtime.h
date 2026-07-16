#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _MSC_VER
#include <intrin.h>
#include <crtdbg.h>
#include <windows.h>
#define mylang_atomic_inc(p) InterlockedIncrement(p)
#define mylang_atomic_dec(p) InterlockedDecrement(p)
#define mylang_atomic_cas(p, n, o) InterlockedCompareExchange(p, n, o)
#else
#include <stdatomic.h>
#define mylang_atomic_inc(p) (atomic_fetch_add(p, 1) + 1)
#define mylang_atomic_dec(p) (atomic_fetch_sub(p, 1) - 1)
#define mylang_atomic_cas(p, n, o) __sync_val_compare_and_swap(p, o, n)
#endif

/* Element kind constants for array helpers.
   The compiler selects the right kind based on the element type. */
#define MYLANG_ELEM_PRIMITIVE  0
#define MYLANG_ELEM_STRUCT     1
#define MYLANG_ELEM_CLASS      2
#define MYLANG_ELEM_INTERFACE  3
#define MYLANG_ELEM_WEAK_CLASS 4
#define MYLANG_ELEM_WEAK_IFACE 5

typedef struct ObjHeaderTag ObjHeader;
typedef struct LeakTraceTag LeakTrace;

typedef struct WeakRef {
    volatile long refcount;
    ObjHeader* obj;
} WeakRef;

struct ObjHeaderTag {
    volatile long refcount;
    uint32_t type_id;
    WeakRef* weak;
    /* Per-class finalizer registered by the compiler.  Called by mylang_release
       before the object's memory is freed. */
    void (*dtor)(void*);
    /* Leak-check fields are always present so that runtime.c and generated code
       share the same header layout regardless of the MYLANG_LEAK_CHECK macro. */
    struct ObjHeaderTag* next;
    struct ObjHeaderTag* prev;
    struct LeakTraceTag* alloc_trace;
};

#define mylang_obj_hdr(ptr) ((ObjHeader*)((char*)(ptr) - sizeof(ObjHeader)))

/* Array value type.  The struct itself is a plain value (lives on the stack or
   inside another value).  The data pointer refers to a separate heap-allocated
   buffer that can be realloc'd without moving this struct. */
typedef struct {
    size_t capacity;
    size_t length;
    void* data;
} MyArray;

/* Builtin String class.  Defined here because it is part of the standard
   library and its layout must be known to both runtime.c and generated code. */
typedef struct String {
    MyArray bytes;
} String;

void _mylang_dtor_String(String* p);
String* mylang_string_new(uint32_t type_id, const char* cstr);
void    mylang_print_string(String* s);
void    mylang_print_i32(int32_t v);

/* Mutable append API on String.  These are the native implementations of the
   String class methods registered in symtab_init. */
void String_append_string(String* thiz, String* s);
void String_append_i32(String* thiz, int32_t v);
void String_append_i64(String* thiz, int64_t v);
void String_append_u32(String* thiz, uint32_t v);
void String_append_u64(String* thiz, uint64_t v);
void String_append_f32(String* thiz, float v);
void String_append_f64(String* thiz, double v);
void String_append_char(String* thiz, int8_t c);

/* Codegen-only helper: append a C string literal to a String without
   allocating a temporary String object. */
void mylang_string_append_cstr(String* thiz, const char* cstr);

/* Stack trace support.  Defined as translation-unit globals in runtime.c;
   macros below are used by generated code. */
typedef struct {
    const char* func;
    const char* file;
    int         line;
} MyFrame;

#define MY_STACK_MAX 128

#ifdef _MSC_VER
#define MY_TL __declspec(thread)
#else
#define MY_TL _Thread_local
#endif

extern MY_TL const char* __my_file;
extern MY_TL int         __my_line;
extern MY_TL MyFrame     __my_stack[MY_STACK_MAX];
extern MY_TL int         __my_depth;

#define MY_LOC(l) do { __my_line = (l); } while(0)
#define MY_PUSH(fn, f, l) do { \
    if (__my_depth < MY_STACK_MAX) { \
        MyFrame* _fr = &__my_stack[__my_depth++]; \
        _fr->func = (fn); _fr->file = (f); _fr->line = (l); \
    } \
} while(0)
#define MY_POP() do { if (__my_depth > 0) __my_depth--; } while(0)

void my_backtrace(void);
void my_panic(const char* msg);

#define MY_CHECK(c, m) do { if (!(c)) my_panic(m); } while(0)

/* Core allocation / refcounting for class and interface objects. */
void* mylang_new_object(size_t sz, uint32_t type_id, void (*dtor)(void*));
void* mylang_retain(void* ptr);
int   mylang_release(void* ptr);

/* Array helpers.  The compiler passes elem_size and elem_kind so that the
   runtime can copy/move/release elements correctly without storing metadata
   in the array header. */
void  mylang_array_init(MyArray* a, size_t count, size_t elem_size);
void  mylang_array_free(MyArray* a, size_t elem_size, int elem_kind);
void  mylang_array_reserve(MyArray* a, size_t new_capacity, size_t elem_size);
void  mylang_array_resize(MyArray* a, size_t new_length, size_t elem_size, int elem_kind);
void  mylang_array_move(MyArray* src, MyArray* dst, size_t elem_size, int elem_kind);
void  mylang_array_copy(const MyArray* src, MyArray* dst, size_t elem_size, int elem_kind);
void* mylang_array_at(MyArray* a, size_t idx, size_t elem_size);
void  mylang_array_push(MyArray* a, size_t elem_size, int elem_kind, const void* value);
void  mylang_array_pop(MyArray* a, size_t elem_size, int elem_kind);
void  mylang_array_clear(MyArray* a, size_t elem_size, int elem_kind);
void  mylang_array_compact(MyArray* a, size_t elem_size);

/* Weak references. */
WeakRef* mylang_weak_init(void* ptr);
WeakRef* mylang_weak_init_owned(void* ptr);
WeakRef* mylang_weak_copy(WeakRef* wr);
void*    mylang_lock(WeakRef* wr);
void     mylang_weak_release(WeakRef* wr);

/* Leak checking (controlled by MYLANG_LEAK_CHECK macro).
   Only ObjHeader-based allocations are tracked here; arrays are tracked
   separately if needed. */
#ifdef MYLANG_LEAK_CHECK
void mylang_leak_insert(ObjHeader* h);
void mylang_leak_remove(ObjHeader* h);
#else
#define mylang_leak_insert(h) ((void)0)
#define mylang_leak_remove(h) ((void)0)
#endif

#endif /* RUNTIME_H */
