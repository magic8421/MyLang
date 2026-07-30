#include "codegen.h"
#include "sema.h"
#include "symtab.h"
#include "util.h"
#include "runtime.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* Forward declarations used by helper functions defined before their
   implementation. */
typedef struct CodegenContext CodegenContext;
static void codegen_expr(CodegenContext* ctx, AstNode* node);
static void codegen_expr_dispatch(CodegenContext* ctx, AstNode* node);
static void codegen_expr_raw(CodegenContext* ctx, AstNode* node);
static void indent_line(CodegenContext* ctx, int indent);
static void emit_array_ptr_expr(CodegenContext* ctx, AstNode* arr_node);
static void codegen_expr_stmt(CodegenContext* ctx, AstNode* node, int indent);
static int is_compound_assign_op(TokenKind k);

#define MAX_CLEANUP 128
#define MAX_SCOPE 64

typedef struct {
    const char* name;
    int         is_weak;
    int         is_interface;
    int         is_interface_array;
    int         interface_array_size;
    int         is_weak_interface;
    int         is_weak_interface_array;
    int         weak_interface_array_size;
    int         is_array;
    int         array_elem_kind;
    char        array_elem_size_expr[NAME_BUF_SIZE];
    int         is_struct_dtor;      /* struct with reference fields */
    char        struct_dtor_name[64];
} CleanupEntry;

#define MAX_LOOP 64

struct CodegenContext {
    FILE* out;
    FILE* header;
    const char* header_include_name;
    const char* source_file;
    char        source_file_escaped[1024];
    /* Per-declaration source file: with imports, each function/method reports
       panics and errors against the file it was defined in. */
    char        current_file[1024];
    char        current_file_escaped[1024];
    Type        return_type;
    int         has_main;
    Type        main_return_type;
    int         codegen_error;
    int         guard_tmp_id;
    CleanupEntry cleanup_entries[MAX_CLEANUP];
    int          cleanup_count;
    int          assign_tmp_id;
    int          subexpr_tmp_id;
    int          fstring_tmp_id;
    int          cleanup_scope_stack[MAX_SCOPE];
    int          cleanup_scope_depth;
    int          last_loc_line;
    int          is_interface_default_method;
    int          no_unowned_check;
    ClassInfo*   current_class;   /* class whose method body is being emitted, NULL outside methods */
    int          current_method_is_static;   /* emitting a static method body (no 'this') */
    int          loop_depth;
    int          loop_entry_cleanup_count[MAX_LOOP];
    int          loop_break_label_id[MAX_LOOP];
    int          loop_continue_label_id[MAX_LOOP];
    int          xor_strings;
    int          xor_str_id;
};

static int s_last_codegen_error = 0;

int codegen_had_error(void) {
    return s_last_codegen_error;
}

extern ClassInfo* class_list;

static void escape_source_file(CodegenContext* ctx, const char* src) {
    size_t i, j;
    if (!src) src = "";
    for (i = 0, j = 0; src[i] != '\0' && j < sizeof(ctx->source_file_escaped) - 2; i++) {
        if (src[i] == '\\' || src[i] == '"') ctx->source_file_escaped[j++] = '\\';
        ctx->source_file_escaped[j++] = src[i];
    }
    ctx->source_file_escaped[j] = '\0';
}

/* Switch the per-declaration source file used by MY_PUSH and error reports.
   Falls back to the root source file when the declaration's token carries
   no filename. */
static void codegen_set_current_file(CodegenContext* ctx, const char* filename) {
    const char* f = (filename && filename[0] != '\0') ? filename : ctx->source_file;
    size_t i, j;
    if (!f) f = "";
    for (i = 0, j = 0; f[i] != '\0' && j < sizeof(ctx->current_file) - 1; i++, j++) {
        ctx->current_file[j] = f[i];
    }
    ctx->current_file[j] = '\0';
    for (i = 0, j = 0; ctx->current_file[i] != '\0' && j < sizeof(ctx->current_file_escaped) - 2; i++) {
        if (ctx->current_file[i] == '\\' || ctx->current_file[i] == '"') ctx->current_file_escaped[j++] = '\\';
        ctx->current_file_escaped[j++] = ctx->current_file[i];
    }
    ctx->current_file_escaped[j] = '\0';
}

static const char* c_base_name(const Type* t) {
    if (t->is_weak || t->is_unowned) return "WeakRef";
    switch (t->type_kind) {
        case TYPE_I8:    return "int8_t";
        case TYPE_I16:   return "int16_t";
        case TYPE_I32:   return "int32_t";
        case TYPE_I64:   return "int64_t";
        case TYPE_U8:    return "uint8_t";
        case TYPE_U16:   return "uint16_t";
        case TYPE_U32:   return "uint32_t";
        case TYPE_U64:   return "uint64_t";
        case TYPE_F32:   return "float";
        case TYPE_F64:   return "double";
        case TYPE_BOOL:  return "int";
        case TYPE_OBJECT: return "void";
        case TYPE_ENUM:  return t->class_name;
        case TYPE_CLASS:
        case TYPE_STRUCT:
        case TYPE_INTERFACE:
            if (t->type_arg_count > 0) {
                if (!t->mangled_name[0]) type_mangled_name((Type*)t);
                return t->mangled_name;
            }
            return t->class_name;
        case TYPE_VOID:  return "void";
        default:         return "int32_t";
    }
}

static const char* class_c_name(const ClassInfo* ci) {
    if (ci->is_instantiation && ci->mangled_name[0]) return ci->mangled_name;
    return ci->name;
}

static PropertyInfo* member_access_property(AstNode* node, ClassInfo** out_ci);

static int expr_is_owned(AstNode* node) {
    if (node && node->ast_kind == AST_MEMBER_ACCESS) {
        /* A reference-typed property read lowers to a getter call, whose
           result is owned like any other call result.  Value-typed property
           reads keep the plain field-access treatment. */
        PropertyInfo* pi = member_access_property(node, NULL);
        if (pi && pi->prop_type.type_kind == TYPE_CLASS) return 1;
    }
    return node && (node->ast_kind == AST_CALL ||
                    node->ast_kind == AST_NEW ||
                    node->ast_kind == AST_STRING_LIT);
}

/* True when the expression already holds an owned (+1) reference for the
   purpose of caller-side retain/release guards around calls.  F-string
   accumulators are created by the preamble and tracked on the cleanup list,
   so they are owned in this sense even though they are not a call/new. */
static int guard_expr_is_owned(AstNode* node) {
    return expr_is_owned(node) || (node && node->ast_kind == AST_FSTRING);
}

/* The 'null' literal has the compile-time-only type TYPE_NULL.  It may be
   assigned to class (including string), interface, and weak references;
   unowned references can never be null. */
static int type_is_null(const Type* t) {
    return t->type_kind == TYPE_NULL;
}

static int type_accepts_null(const Type* t) {
    if (t->is_array || t->is_unowned) return 0;
    return t->type_kind == TYPE_CLASS || t->type_kind == TYPE_INTERFACE ||
           t->type_kind == TYPE_OBJECT;
}

/* Strict bool rule: bool and numeric types do not implicitly convert.
   True when exactly one side is bool. */
static int bool_mismatch(const Type* dst, const Type* src) {
    return (dst->type_kind == TYPE_BOOL) != (src->type_kind == TYPE_BOOL);
}

/* Strict enum rule: enums do not implicitly convert to or from any other
   type, and two different enum types do not convert between each other.
   Cross the boundary with an explicit 'as' cast instead. */
static int enum_mismatch(const Type* dst, const Type* src) {
    int de = dst->type_kind == TYPE_ENUM;
    int se = src->type_kind == TYPE_ENUM;
    if (de != se) return 1;
    return de && strcmp(dst->class_name, src->class_name) != 0;
}

/* Reference-like types: class, interface, object (weak/unowned class and
   interface are included because they are represented as pointers/fat
   pointers).  Arrays are handled separately. */
static int type_is_reference(const Type* t) {
    return t->type_kind == TYPE_CLASS || t->type_kind == TYPE_INTERFACE ||
           t->type_kind == TYPE_OBJECT;
}

/* Class lookup that also materialises generic instantiations. */
static ClassInfo* class_info_for_type(Type* t) {
    if (t->type_arg_count > 0) return symtab_instantiate_class_from_type(t);
    return symtab_find_class(t->class_name);
}

static int class_implements(ClassInfo* ci, const char* iname) {
    if (!ci) return 0;
    int i;
    for (i = 0; i < ci->impl_count && i < MAX_IMPL; i++) {
        if (strcmp(ci->impl_names[i], iname) == 0) return 1;
    }
    return 0;
}

/* C identifier prefix for methods of the class: mangled for generic
   instantiations, plain name otherwise. */
static const char* class_c_prefix(ClassInfo* ci) {
    return ci->mangled_name[0] ? ci->mangled_name : ci->name;
}

/* Structs that (transitively) own reference-counted shares get compiler-
   generated _mylang_retain_S / _mylang_release_S hooks: copies retain each
   reference field and scope exit releases them. */
static int struct_has_ref_fields(const Type* t) {
    if (t->type_kind != TYPE_STRUCT) return 0;
    StructInfo* si = symtab_find_struct(t->class_name);
    return si && si->has_ref_fields;
}

/* Arrays of such structs are rejected: MyArray is type-erased and cannot run
   per-element retain/release hooks yet. */
static int type_is_ref_struct_array(const Type* t) {
    if (!t->is_array) return 0;
    Type et = *t;
    et.is_array = 0;
    et.array_size = 0;
    return struct_has_ref_fields(&et);
}

/* Detect the special case of calling .lock() on an unowned reference.  The
   codegen_call path emits a dedicated error message for this; we must not
   shadow it with a generic type-mismatch error in the variable-init or
   assignment paths.  Callers have already resolved the source expression. */
static int expr_is_unowned_lock(AstNode* node) {
    if (!node || node->ast_kind != AST_CALL || node->ast_child_count < 1) return 0;
    AstNode* mem = node->ast_children[0];
    if (!mem || mem->ast_kind != AST_MEMBER_ACCESS) return 0;
    if (strcmp(mem->ast_token.text, "lock") != 0) return 0;
    AstNode* obj = mem->ast_children[0];
    if (!obj) return 0;
    return obj->ast_resolved_type.is_unowned;
}
/* Emit a codegen error with a VS Code-compatible location prefix.
   The format is "path(line,col): error: message" so that problem matchers
   (including MSVC style) can jump to the source location. */
static void codegen_report_error(CodegenContext* ctx, int line, int col, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s(%d,%d): error: ", ctx->current_file[0] ? ctx->current_file : "<unknown>", line, col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    ctx->codegen_error = 1;
}


/* Access control: a private class member is visible only inside methods of
   the same class (any instance, C++ style). */
static int member_visible(CodegenContext* ctx, const char* owner, int is_private) {
    if (!is_private) return 1;
    return ctx->current_class && strcmp(ctx->current_class->name, owner) == 0;
}

/* C identifier for a const: plain name for top-level consts, Class_NAME for
   static class members (the Class_method naming convention). */
static void const_c_name(const ConstInfo* ci, char* out, size_t out_size) {
    if (ci->owner_class[0]) {
        snprintf(out, out_size, "%s_%s", ci->owner_class, ci->name);
    } else {
        snprintf(out, out_size, "%s", ci->name);
    }
}

/* Integer types (bitwise operands); bool is intentionally excluded. */
static int type_is_integer(const Type* t) {
    return t->type_kind == TYPE_I8 || t->type_kind == TYPE_I16 ||
           t->type_kind == TYPE_I32 || t->type_kind == TYPE_I64 ||
           t->type_kind == TYPE_U8 || t->type_kind == TYPE_U16 ||
           t->type_kind == TYPE_U32 || t->type_kind == TYPE_U64;
}

/* Primitive numeric types (arithmetic compound assignment). */
static int type_is_numeric(const Type* t) {
    return type_is_integer(t) || t->type_kind == TYPE_F32 || t->type_kind == TYPE_F64;
}

static int is_bit_compound_op(TokenKind k) {
    return k == TOK_AMP_ASSIGN || k == TOK_PIPE_ASSIGN || k == TOK_CARET_ASSIGN ||
           k == TOK_SHL_ASSIGN || k == TOK_SHR_ASSIGN;
}

static const char* compound_op_text(TokenKind k) {
    switch (k) {
        case TOK_PLUS_ASSIGN:  return "+";
        case TOK_MINUS_ASSIGN: return "-";
        case TOK_STAR_ASSIGN:  return "*";
        case TOK_SLASH_ASSIGN: return "/";
        case TOK_AMP_ASSIGN:   return "&";
        case TOK_PIPE_ASSIGN:  return "|";
        case TOK_CARET_ASSIGN: return "^";
        case TOK_SHL_ASSIGN:   return "<<";
        case TOK_SHR_ASSIGN:   return ">>";
        default:               return "?";
    }
}

static void c_weak_interface_name(const Type* t, char* buf, size_t bufsz) {
    int n = snprintf(buf, bufsz, "Weak%s", t->class_name);
    CHECK_SNPRINTF(n, bufsz, "weak interface name too long");
}

/* Helpers for the value-type MyArray representation. */

static Type array_elem_type(const Type* arr_type) {
    Type et = *arr_type;
    et.is_array = 0;
    et.array_size = 0;
    return et;
}

static void c_array_elem_type_name(const Type* arr_type, char* buf, int bufsz) {
    Type et = array_elem_type(arr_type);
    int n;
    if (et.is_weak && et.type_kind == TYPE_INTERFACE) {
        char winame[128];
        c_weak_interface_name(&et, winame, sizeof(winame));
        n = snprintf(buf, bufsz, "%s", winame);
    } else if (et.is_weak) {
        /* weak class arrays store WeakRef pointers */
        n = snprintf(buf, bufsz, "WeakRef*");
    } else if (et.type_kind == TYPE_CLASS || et.type_kind == TYPE_OBJECT) {
        /* class/object arrays store pointers to objects */
        n = snprintf(buf, bufsz, "%s*", c_base_name(&et));
    } else {
        n = snprintf(buf, bufsz, "%s", c_base_name(&et));
    }
    CHECK_SNPRINTF(n, (size_t)bufsz, "array element type name too long");
}

static void array_elem_size_expr(const Type* arr_type, char* buf, int bufsz) {
    Type et = array_elem_type(arr_type);
    int n;
    if ((et.type_kind == TYPE_CLASS || et.type_kind == TYPE_OBJECT) && !et.is_weak) {
        n = snprintf(buf, bufsz, "sizeof(void*)");
    } else if (et.type_kind == TYPE_INTERFACE && !et.is_weak) {
        n = snprintf(buf, bufsz, "sizeof(%s)", c_base_name(&et));
    } else if (et.is_weak && et.type_kind == TYPE_INTERFACE) {
        char winame[128];
        c_weak_interface_name(&et, winame, sizeof(winame));
        n = snprintf(buf, bufsz, "sizeof(%s)", winame);
    } else if (et.is_weak) {
        n = snprintf(buf, bufsz, "sizeof(WeakRef*)");
    } else {
        n = snprintf(buf, bufsz, "sizeof(%s)", c_base_name(&et));
    }
    CHECK_SNPRINTF(n, (size_t)bufsz, "array element size expression too long");
}

static int array_elem_kind(const Type* arr_type) {
    Type et = array_elem_type(arr_type);
    if (et.is_weak) {
        if (et.type_kind == TYPE_INTERFACE) return MYLANG_ELEM_WEAK_IFACE;
        return MYLANG_ELEM_WEAK_CLASS;
    }
    if (et.type_kind == TYPE_INTERFACE) return MYLANG_ELEM_INTERFACE;
    if (et.type_kind == TYPE_CLASS || et.type_kind == TYPE_OBJECT) return MYLANG_ELEM_CLASS;
    if (et.type_kind == TYPE_STRUCT) return MYLANG_ELEM_STRUCT;
    return MYLANG_ELEM_PRIMITIVE;
}

static void emit_array_data_expr(CodegenContext* ctx, AstNode* arr_node) {
    /* codegen_expr already dereferences ref parameters, so the resulting
       expression is always a MyArray value. */
    fprintf(ctx->out, "(");
    codegen_expr(ctx, arr_node);
    fprintf(ctx->out, ")");
    fprintf(ctx->out, ".data");
}

static void emit_array_length_expr(CodegenContext* ctx, AstNode* arr_node) {
    fprintf(ctx->out, "(");
    codegen_expr(ctx, arr_node);
    fprintf(ctx->out, ")");
    fprintf(ctx->out, ".length");
}

static void preinstantiate_generic_types(AstNode* node) {
    if (!node) return;
    if (node->ast_resolved_type.type_kind == TYPE_CLASS && node->ast_resolved_type.type_arg_count > 0) {
        symtab_instantiate_class_from_type(&node->ast_resolved_type);
    }
    int i;
    for (i = 0; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
        preinstantiate_generic_types(node->ast_children[i]);
    }
    preinstantiate_generic_types(node->next);
}

static void c_type_str(const Type* t, char* buf, int bufsz) {
    int n;
    if (t->is_array) {
        /* dynamic array is a value-type MyArray; ref parameters add the pointer. */
        n = snprintf(buf, bufsz, "MyArray");
    } else if (t->is_weak && t->type_kind == TYPE_INTERFACE) {
        char winame[128];
        c_weak_interface_name(t, winame, sizeof(winame));
        n = snprintf(buf, bufsz, "%s", winame);
    } else if (t->type_kind == TYPE_INTERFACE) {
        /* interface fat pointer struct */
        n = snprintf(buf, bufsz, "%s", t->class_name);
    } else if (t->type_kind == TYPE_CLASS || t->is_pointer) {
        n = snprintf(buf, bufsz, "%s*", c_base_name(t));
    } else {
        n = snprintf(buf, bufsz, "%s", c_base_name(t));
    }
    CHECK_SNPRINTF(n, (size_t)bufsz, "type name too long");
}


/* Header generation helpers ------------------------------------------------ */

static void header_guard_name(const char* header_name, char* buf, size_t size) {
    size_t i, j;
    int n = snprintf(buf, size, "MYLANG_");
    j = (size_t)n < size ? (size_t)n : 0;
    for (i = 0; header_name[i] && j + 1 < size; i++) {
        char c = header_name[i];
        if (c >= 'a' && c <= 'z') {
            buf[j++] = (char)(c - 'a' + 'A');
        } else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            buf[j++] = c;
        } else {
            buf[j++] = '_';
        }
    }
    buf[j] = '\0';
}

static void emit_header_preamble(CodegenContext* ctx) {
    char guard[256];
    header_guard_name(ctx->header_include_name, guard, sizeof(guard));
    fprintf(ctx->header, "#ifndef %s\n#define %s\n\n", guard, guard);
    fprintf(ctx->header, "#include \"runtime.h\"\n\n");
    fprintf(ctx->header, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");
}

static void emit_header_postamble(CodegenContext* ctx) {
    char guard[256];
    header_guard_name(ctx->header_include_name, guard, sizeof(guard));
    fprintf(ctx->header, "\n#ifdef __cplusplus\n}\n#endif\n\n");
    fprintf(ctx->header, "#endif /* %s */\n", guard);
}

static int is_builtin_class(const char* name) {
    return strcmp(name, "String") == 0;
}

static void emit_interface_forward_decls(CodegenContext* ctx) {
    FILE* h = ctx->header;
    extern InterfaceInfo* interface_list;
    InterfaceInfo* ii = interface_list;
    while (ii) {
        /* Forward-declare the vtable type so the fat-pointer struct can use a
           pointer to it.  The vtable definition itself is emitted after all
           class/struct layouts are known, because its method pointers may use
           those value types by value. */
        fprintf(h, "typedef struct %sVTable %sVTable;\n", ii->name, ii->name);
        fprintf(h, "typedef struct %s {\n", ii->name);
        fprintf(h, "    void* data;\n");
        fprintf(h, "    const %sVTable* vtable;\n", ii->name);
        fprintf(h, "} %s;\n\n", ii->name);
        fprintf(h, "typedef struct Weak%s {\n", ii->name);
        fprintf(h, "    WeakRef* wr;\n");
        fprintf(h, "    const %sVTable* vt;\n", ii->name);
        fprintf(h, "} Weak%s;\n\n", ii->name);
        ii = ii->next;
    }
}

static void emit_header_forward_decls(CodegenContext* ctx) {
    FILE* h = ctx->header;
    ClassInfo* ci = class_list;
    while (ci) {
        if (!ci->is_generic && !is_builtin_class(class_c_name(ci))) {
            const char* cn = class_c_name(ci);
            fprintf(h, "typedef struct %s %s;\n", cn, cn);
        }
        ci = ci->next;
    }
    emit_interface_forward_decls(ctx);
    fprintf(h, "\n");
}

static void emit_class_struct_def_to_header(CodegenContext* ctx, ClassInfo* ci, const char* class_c) {
    FILE* h = ctx->header;
    if (is_builtin_class(class_c)) return;
    fprintf(h, "typedef struct %s {\n", class_c);
    if (ci->field_count == 0) {
        fprintf(h, "    char _pad;\n");
    } else {
        int i;
        for (i = 0; i < ci->field_count; i++) {
            char ftype_buf[128];
            if (ci->field_types[i].type_kind == TYPE_CLASS &&
                ci->field_types[i].type_arg_count == 0 &&
                strcmp(ci->field_types[i].class_name, ci->name) == 0) {
                int n = snprintf(ftype_buf, sizeof(ftype_buf), "struct %s*", ci->field_types[i].class_name);
                CHECK_SNPRINTF(n, (size_t)sizeof(ftype_buf), "field type name too long");
            } else {
                c_type_str(&ci->field_types[i], ftype_buf, sizeof(ftype_buf));
            }
            fprintf(h, "    %s %s;\n", ftype_buf, ci->field_names[i]);
        }
    }
    fprintf(h, "} %s;\n\n", class_c);
}

static void emit_struct_def_to_header(CodegenContext* ctx, StructInfo* si) {
    FILE* h = ctx->header;
    fprintf(h, "typedef struct %s {\n", si->name);
    if (si->field_count == 0) {
        fprintf(h, "    char _pad;\n");
    } else {
        int i;
        for (i = 0; i < si->field_count; i++) {
            char ftype_buf[128];
            c_type_str(&si->field_types[i], ftype_buf, sizeof(ftype_buf));
            fprintf(h, "    %s %s;\n", ftype_buf, si->field_names[i]);
        }
    }
    fprintf(h, "} %s;\n\n", si->name);
}

/* Emit a struct's C definition after the definitions of all structs it
   contains, so nested struct fields always refer to a completed type. */
static void emit_struct_def_ordered(CodegenContext* ctx, StructInfo* si) {
    if (!si || si->emitted_in_header) return;
    int i;
    for (i = 0; i < si->field_count; i++) {
        Type* ft = &si->field_types[i];
        if (ft->type_kind == TYPE_STRUCT) {
            emit_struct_def_ordered(ctx, symtab_find_struct(ft->class_name));
        }
    }
    si->emitted_in_header = 1;
    emit_struct_def_to_header(ctx, si);
}

/* Emit the C definitions of all struct types a class embeds by value, so
   the class struct definition can be emitted regardless of declaration
   order.  Array fields embed only MyArray and need no dependency. */
static void emit_class_field_struct_deps(CodegenContext* ctx, ClassInfo* ci) {
    int i;
    for (i = 0; i < ci->field_count; i++) {
        Type* ft = &ci->field_types[i];
        if (!ft->is_array && ft->type_kind == TYPE_STRUCT) {
            emit_struct_def_ordered(ctx, symtab_find_struct(ft->class_name));
        }
    }
}

/* Emit one C typedef per registered enum:
   typedef enum Key { Key_Up = 0, ... } Key;
   Variant C names follow the Class_method naming convention. */
static void emit_enum_typedefs(CodegenContext* ctx) {
    FILE* h = ctx->header;
    EnumInfo* ei = symtab_first_enum();
    while (ei) {
        fprintf(h, "typedef enum %s {\n", ei->name);
        int i;
        for (i = 0; i < ei->variant_count; i++) {
            fprintf(h, "    %s_%s = %ld%s\n", ei->name, ei->variant_names[i],
                    ei->variant_values[i], i + 1 < ei->variant_count ? "," : "");
        }
        fprintf(h, "} %s;\n\n", ei->name);
        ei = ei->next;
    }
}

static void emit_header_type_ids(CodegenContext* ctx) {
    FILE* h = ctx->header;
    ClassInfo* ci = class_list;
    while (ci) {
        if (!ci->is_generic) {
            fprintf(h, "#define MYLANG_TID_%s %u\n", class_c_name(ci), (unsigned)ci->type_id);
        }
        ci = ci->next;
    }
    {
        StructInfo* si = symtab_first_struct();
        while (si) {
            fprintf(h, "#define MYLANG_TID_%s %u\n", si->name, (unsigned)si->type_id);
            si = si->next;
        }
    }
    {
        extern InterfaceInfo* interface_list;
        InterfaceInfo* ii = interface_list;
        while (ii) {
            fprintf(h, "#define MYLANG_TID_%s %u\n", ii->name, (unsigned)ii->type_id);
            ii = ii->next;
        }
    }
    fprintf(h, "\n");
}

static void emit_header_destructor_prototypes(CodegenContext* ctx) {
    FILE* h = ctx->header;
    ClassInfo* ci = class_list;
    while (ci) {
        if (!ci->is_generic && !is_builtin_class(class_c_name(ci))) {
            const char* cc = class_c_name(ci);
            fprintf(h, "void _mylang_dtor_%s(%s* p);\n", cc, cc);
        }
        ci = ci->next;
    }
    fprintf(h, "\n");
}

static void emit_function_signature(FILE* out, const char* ret_cstr, const char* func_name,
                                    int param_count, const char param_names[][NAME_BUF_SIZE], const Type param_types[]) {
    fprintf(out, "%s %s(", ret_cstr, func_name);
    int i;
    for (i = 0; i < param_count; i++) {
        if (i > 0) fprintf(out, ", ");
        char pt[128];
        c_type_str(&param_types[i], pt, sizeof(pt));
        if (param_types[i].is_ref) {
            fprintf(out, "%s* %s", pt, param_names[i]);
        } else {
            fprintf(out, "%s %s", pt, param_names[i]);
        }
    }
    fprintf(out, ")");
}

static void emit_header_function_prototypes(CodegenContext* ctx) {
    FILE* h = ctx->header;
    FuncInfo* f = symtab_first_func();
    while (f) {
        if (strcmp(f->name, "main") == 0 || f->is_builtin) { f = f->next; continue; }
        char ret[128];
        c_type_str(&f->return_type, ret, sizeof(ret));
        emit_function_signature(h, ret, f->name, f->param_count, f->param_names, f->param_types);
        fprintf(h, ";\n");
        f = f->next;
    }
    fprintf(h, "\n");
}

static void emit_header_method_prototypes(CodegenContext* ctx) {
    FILE* h = ctx->header;
    ClassInfo* ci = class_list;
    while (ci) {
        if (ci->is_generic) { ci = ci->next; continue; }
        const char* cc = class_c_name(ci);
        MethodInfo* m = ci->methods;
        while (m) {
            char ret[128];
            char name_buf[256];
            c_type_str(&m->return_type, ret, sizeof(ret));
            int n = snprintf(name_buf, sizeof(name_buf), "%s_%s", cc, m->name);
            CHECK_SNPRINTF(n, (size_t)sizeof(name_buf), "method name too long");
            fprintf(h, "%s %s(%s* thiz", ret, name_buf, cc);
            {
                int i;
                for (i = 0; i < m->param_count; i++) {
                    char pt[128];
                    c_type_str(&m->param_types[i], pt, sizeof(pt));
                    if (m->param_types[i].is_ref) {
                        fprintf(h, ", %s* %s", pt, m->param_names[i]);
                    } else {
                        fprintf(h, ", %s %s", pt, m->param_names[i]);
                    }
                }
            }
            fprintf(h, ");\n");
            m = m->next;
        }
        ci = ci->next;
    }
    StructInfo* si = symtab_first_struct();
    while (si) {
        MethodInfo* m = si->methods;
        while (m) {
            char ret[128];
            char name_buf[256];
            c_type_str(&m->return_type, ret, sizeof(ret));
            int n = snprintf(name_buf, sizeof(name_buf), "%s_%s", si->name, m->name);
            CHECK_SNPRINTF(n, (size_t)sizeof(name_buf), "method name too long");
            fprintf(h, "%s %s(%s* thiz", ret, name_buf, si->name);
            {
                int i;
                for (i = 0; i < m->param_count; i++) {
                    char pt[128];
                    c_type_str(&m->param_types[i], pt, sizeof(pt));
                    if (m->param_types[i].is_ref) {
                        fprintf(h, ", %s* %s", pt, m->param_names[i]);
                    } else {
                        fprintf(h, ", %s %s", pt, m->param_names[i]);
                    }
                }
            }
            fprintf(h, ");\n");
            m = m->next;
        }
        if (si->has_ref_fields) {
            fprintf(h, "void _mylang_retain_%s(%s* p);\n", si->name, si->name);
            fprintf(h, "void _mylang_release_%s(%s* p);\n", si->name, si->name);
        }
        si = si->next;
    }
}

/* Type resolution lives in sema.c (sema_resolve_type, sema_static_call_method);
   these thin wrappers keep the existing codegen call sites unchanged.  Nodes
   created by codegen-time lowering or generic instantiation carry no cache
   and resolve on demand exactly as before. */
static Type resolve_type(AstNode* node) { return sema_resolve_type(node); }

static MethodInfo* static_call_method(AstNode* callee, ClassInfo** out_ci) {
    return sema_static_call_method(callee, out_ci);
}

static void codegen_expr(CodegenContext* ctx, AstNode* node);

static void codegen_binary(CodegenContext* ctx, AstNode* node) {
    TokenKind op = node->ast_token.kind;
    Type lt = resolve_type(node->ast_children[0]);
    Type rt = resolve_type(node->ast_children[1]);

    if (type_is_null(&lt) || type_is_null(&rt)) {
        /* null may only be compared for (in)equality. */
        if (op != TOK_EQ && op != TOK_NE) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "operator '%s' not allowed with null", node->ast_token.text);
            fprintf(ctx->out, "0 /* invalid null comparison */");
            return;
        }
        /* Pick the non-null side to decide the comparison shape. */
        Type nt = type_is_null(&lt) ? rt : lt;
        AstNode* ne = type_is_null(&lt) ? node->ast_children[1] : node->ast_children[0];
        fprintf(ctx->out, "(");
        if (nt.type_kind == TYPE_INTERFACE && !nt.is_weak) {
            codegen_expr(ctx, ne);
            fprintf(ctx->out, ".data %s NULL)", node->ast_token.text);
        } else if (nt.type_kind == TYPE_INTERFACE && nt.is_weak) {
            codegen_expr(ctx, ne);
            fprintf(ctx->out, ".wr %s NULL)", node->ast_token.text);
        } else if (nt.type_kind == TYPE_CLASS || nt.type_kind == TYPE_OBJECT ||
                   type_is_null(&nt)) {
            if (type_is_null(&lt)) {
                fprintf(ctx->out, "NULL %s ", node->ast_token.text);
                codegen_expr(ctx, ne);
                fprintf(ctx->out, ")");
            } else {
                codegen_expr(ctx, ne);
                fprintf(ctx->out, " %s NULL)", node->ast_token.text);
            }
        } else {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot compare '%s' with null", type_name(&nt));
            fprintf(ctx->out, "0 /* invalid null comparison */");
        }
        return;
    }

    /* Reference-like types may only be compared with other reference-like types
       (or null, handled above).  Comparing a class/interface/object with a
       primitive value such as 0 is not allowed. */
    if (op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_LE ||
        op == TOK_GT || op == TOK_GE) {
        int lhs_ref = type_is_reference(&lt);
        int rhs_ref = type_is_reference(&rt);
        if ((lhs_ref || rhs_ref) && !(lhs_ref && rhs_ref)) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                    "cannot compare '%s' with '%s'", type_name(&lt), type_name(&rt));
            fprintf(ctx->out, "0 /* invalid reference comparison */");
            return;
        }
    }

    /* Enum operands: only == and != between two values of the same enum type
       are allowed; arithmetic and relational operators are rejected. */
    if (lt.type_kind == TYPE_ENUM || rt.type_kind == TYPE_ENUM) {
        int enum_ok = (op == TOK_EQ || op == TOK_NE) &&
                      lt.type_kind == TYPE_ENUM && rt.type_kind == TYPE_ENUM &&
                      strcmp(lt.class_name, rt.class_name) == 0;
        if (!enum_ok) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                    "operator '%s' not allowed for operands of type '%s' and '%s'",
                    node->ast_token.text, type_name(&lt), type_name(&rt));
            fprintf(ctx->out, "0 /* invalid enum operands */");
            return;
        }
    }

    if (op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET ||
        op == TOK_SHL || op == TOK_SHR) {
        /* Bitwise operators accept integer operands only. */
        if (!type_is_integer(&lt) || !type_is_integer(&rt)) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "operator '%s' requires integer operands", node->ast_token.text);
            fprintf(ctx->out, "0 /* invalid bitwise operands */");
            return;
        }
    }

    if (op == TOK_EQ || op == TOK_NE) {
        /* Strong string == / != is value comparison (C# style), backed by
           String_equals.  Comparisons with null take the null path above and
           never reach here; weak/unowned strings keep identity comparison. */
        if (lt.type_kind == TYPE_CLASS && rt.type_kind == TYPE_CLASS &&
            !lt.is_weak && !lt.is_unowned && !rt.is_weak && !rt.is_unowned &&
            strcmp(lt.class_name, "String") == 0 && strcmp(rt.class_name, "String") == 0) {
            if (op == TOK_NE) fprintf(ctx->out, "!");
            fprintf(ctx->out, "String_equals(");
            codegen_expr(ctx, node->ast_children[0]);
            fprintf(ctx->out, ", ");
            codegen_expr(ctx, node->ast_children[1]);
            fprintf(ctx->out, ")");
            return;
        }
        if (lt.type_kind == TYPE_INTERFACE && rt.type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "(");
            codegen_expr(ctx, node->ast_children[0]);
            fprintf(ctx->out, ".data %s ", node->ast_token.text);
            codegen_expr(ctx, node->ast_children[1]);
            fprintf(ctx->out, ".data)");
            return;
        }
    }
    fprintf(ctx->out, "(");
    codegen_expr(ctx, node->ast_children[0]);
    fprintf(ctx->out, " %s ", node->ast_token.text);
    codegen_expr(ctx, node->ast_children[1]);
    fprintf(ctx->out, ")");
}

static void codegen_unary(CodegenContext* ctx, AstNode* node) {
    if (node->ast_token.kind == TOK_TILDE) {
        Type t = resolve_type(node->ast_children[0]);
        if (!type_is_integer(&t)) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "operator '~' requires an integer operand");
            fprintf(ctx->out, "0 /* invalid bitwise operand */");
            return;
        }
    }
    fprintf(ctx->out, "%s", node->ast_token.text);
    codegen_expr(ctx, node->ast_children[0]);
}

static int type_is_ref(const Type* t) {
    return t->is_ref;
}

/* Emit a single argument.  If the callee expects a ref parameter, only a
   local variable identifier is allowed; it is passed as &var, or as the raw
   pointer if var itself is already a ref parameter. */
static void codegen_call_arg(CodegenContext* ctx, AstNode* arg, const Type* param_type) {
    if (param_type->is_array && !type_is_ref(param_type)) {
        codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "array arguments must be passed by ref");
        fprintf(ctx->out, "/* invalid array arg */");
        return;
    }
    if (type_is_ref(param_type)) {
        if (arg->ast_kind != AST_REF_ARG) {
            codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "missing 'ref' keyword for ref parameter");
            fprintf(ctx->out, "0 /* missing ref keyword */");
            return;
        }
        AstNode* var = arg->ast_children[0];
        if (!var || var->ast_kind != AST_IDENT) {
            codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "ref argument must be a local variable");
            fprintf(ctx->out, "0 /* invalid ref argument */");
            return;
        }
        SymEntry* e = symtab_lookup(var->ast_token.text);
        if (!e) {
            codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "ref argument must be a local variable");
            fprintf(ctx->out, "0 /* invalid ref argument */");
            return;
        }
        if (e->type.is_const) {
            codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "cannot pass const variable '%s' to ref parameter", var->ast_token.text);
            fprintf(ctx->out, "0 /* const ref argument */");
            return;
        }
        if (type_is_ref(&e->type)) {
            fprintf(ctx->out, "%s", var->ast_token.text);
        } else {
            fprintf(ctx->out, "&%s", var->ast_token.text);
        }
    } else if (arg->ast_kind == AST_REF_ARG) {
        codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "'ref' argument requires a ref parameter");
        fprintf(ctx->out, "0 /* invalid ref argument */");
    } else {
        resolve_type(arg);
        Type at = arg->ast_resolved_type;

        if (type_is_null(&at)) {
            /* null argument: the shape depends on the parameter type. */
            if (param_type->is_unowned) {
                codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "cannot pass null to unowned parameter");
                fprintf(ctx->out, "0 /* invalid null argument */");
            } else if (param_type->is_weak && param_type->type_kind == TYPE_INTERFACE) {
                char winame[128];
                c_weak_interface_name(param_type, winame, sizeof(winame));
                fprintf(ctx->out, "(%s){ NULL, NULL }", winame);
            } else if (param_type->type_kind == TYPE_INTERFACE) {
                fprintf(ctx->out, "(%s){ NULL, NULL }", param_type->class_name);
            } else if (param_type->type_kind == TYPE_CLASS ||
                       param_type->type_kind == TYPE_OBJECT ||
                       param_type->type_kind == TYPE_VOID) {
                /* class (including weak class and string) and object; TYPE_VOID
                   means the callee signature is unknown, so pass NULL as-is. */
                fprintf(ctx->out, "NULL");
            } else {
                codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "cannot pass null to '%s' parameter", type_name(param_type));
                fprintf(ctx->out, "0 /* invalid null argument */");
            }
            return;
        }

        /* Strict bool/enum rules at the call boundary. */
        if (param_type->type_kind != TYPE_VOID &&
            (bool_mismatch(param_type, &at) || enum_mismatch(param_type, &at))) {
            codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "cannot pass '%s' to '%s' parameter", type_name(&at), type_name(param_type));
            fprintf(ctx->out, "0 /* invalid bool argument */");
            return;
        }

        /* A class parameter does not accept object; cast with 'as' first. */
        if (param_type->type_kind == TYPE_CLASS && at.type_kind == TYPE_OBJECT) {
            codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "cannot pass 'object' to '%s' parameter; cast with 'as' first", type_name(param_type));
            fprintf(ctx->out, "0 /* invalid object argument */");
            return;
        }

        /* object parameter: an interface argument contributes its .data
           pointer; class and object arguments pass through unchanged. */
        if (param_type->type_kind == TYPE_OBJECT) {            if (at.type_kind == TYPE_INTERFACE && !at.is_weak) {
                fprintf(ctx->out, "(");
                codegen_expr(ctx, arg);
                fprintf(ctx->out, ").data");
            } else if (at.type_kind == TYPE_CLASS || at.type_kind == TYPE_OBJECT) {
                codegen_expr(ctx, arg);
            } else {
                codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "cannot pass '%s' to 'object' parameter", type_name(&at));
                fprintf(ctx->out, "0 /* invalid object argument */");
            }
            return;
        }
    }
    if (param_type->type_kind == TYPE_INTERFACE && !param_type->is_weak) {
        resolve_type(arg);
        Type rt = arg->ast_resolved_type;
        if (rt.type_kind == TYPE_CLASS) {
            fprintf(ctx->out, "(%s){ (void*)", param_type->class_name);
            codegen_expr(ctx, arg);
            fprintf(ctx->out, ", &%s_%s_vtable }", rt.class_name, param_type->class_name);
            return;
        }
    }
    if (param_type->is_weak && param_type->type_kind == TYPE_INTERFACE) {
        resolve_type(arg);
        Type rt = arg->ast_resolved_type;
        if (rt.is_weak && rt.type_kind == TYPE_INTERFACE) {
            /* weak interface -> weak interface: struct copy */
            codegen_expr(ctx, arg);
        } else if (rt.type_kind == TYPE_INTERFACE) {
            /* strong interface -> weak interface */
            if (expr_is_owned(arg)) {
                fprintf(ctx->out, "mylang_weakify_%s_owned(", param_type->class_name);
                codegen_expr(ctx, arg);
                fprintf(ctx->out, ")");
            } else {
                fprintf(ctx->out, "mylang_weakify_%s(", param_type->class_name);
                codegen_expr(ctx, arg);
                fprintf(ctx->out, ")");
            }
        } else if (rt.type_kind == TYPE_CLASS) {
            /* class -> weak interface */
            if (expr_is_owned(arg)) {
                fprintf(ctx->out, "mylang_weakify_%s_from_ptr_owned(", param_type->class_name);
                codegen_expr(ctx, arg);
                fprintf(ctx->out, ", &%s_%s_vtable)", rt.class_name, param_type->class_name);
            } else {
                fprintf(ctx->out, "mylang_weakify_%s_from_ptr(", param_type->class_name);
                codegen_expr(ctx, arg);
                fprintf(ctx->out, ", &%s_%s_vtable)", rt.class_name, param_type->class_name);
            }
        } else {
            codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "cannot pass this argument to weak interface parameter");
            fprintf(ctx->out, "/* invalid weak interface arg */");
        }
    } else if (param_type->is_weak || param_type->is_unowned) {
        resolve_type(arg);
        if (arg->ast_resolved_type.type_kind == TYPE_CLASS &&
            !arg->ast_resolved_type.is_weak && !arg->ast_resolved_type.is_unowned) {
            fprintf(ctx->out, "mylang_weak_init(");
            codegen_expr(ctx, arg);
            fprintf(ctx->out, ")");
        } else if (arg->ast_resolved_type.is_weak || arg->ast_resolved_type.is_unowned) {
            fprintf(ctx->out, "mylang_weak_copy(");
            codegen_expr_raw(ctx, arg);
            fprintf(ctx->out, ")");
        } else {
            codegen_expr(ctx, arg);
        }
    } else {
        codegen_expr(ctx, arg);
    }
}

static int is_array_method_name(const char* s) {
    return strcmp(s, "push") == 0 || strcmp(s, "pop") == 0 ||
           strcmp(s, "reserve") == 0 || strcmp(s, "resize") == 0 ||
           strcmp(s, "clear") == 0 || strcmp(s, "compact") == 0 ||
           strcmp(s, "length") == 0 || strcmp(s, "capacity") == 0 ||
           strcmp(s, "move_to") == 0 || strcmp(s, "copy_to") == 0;
}

/* Counts the arguments of a call node (the children[1] next-chain). */
static int count_call_args(AstNode* node) {
    int count = 0;
    AstNode* a = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
    while (a) {
        count++;
        a = a->next;
    }
    return count;
}

/* Appends clones of the stored default-value literals for any missing
   trailing arguments of a user call (free function, class/struct/interface
   method).  Filled arguments then flow through the normal argument paths
   (guarded temporaries for owned strings, per-argument conversions) exactly
   like source-written arguments.  Idempotent: a call that already supplies
   all parameters is left unchanged. */
static void materialize_call_defaults(AstNode* node) {
    if (!node || node->ast_kind != AST_CALL || node->ast_child_count < 1) return;
    AstNode* callee = node->ast_children[0];
    int param_count = 0;
    AstNode* const* param_defaults = NULL;
    if (callee->ast_kind == AST_MEMBER_ACCESS) {
        AstNode* obj = callee->ast_children[0];
        const char* mname = callee->ast_token.text;
        MethodInfo* smi = static_call_method(callee, NULL);
        if (smi && smi->method_is_static) {
            /* Static call via the class name. */
            param_count = smi->param_count;
            param_defaults = smi->param_defaults;
        } else {
        resolve_type(obj);
        Type* ot = &obj->ast_resolved_type;
        if (ot->is_array) return;   /* array methods have fixed signatures */
        if (strcmp(mname, "lock") == 0 && (ot->is_weak || ot->is_unowned)) return;
        if (ot->type_kind == TYPE_CLASS) {
            ClassInfo* ci = ot->type_arg_count > 0
                ? symtab_instantiate_class_from_type(ot)
                : symtab_find_class(ot->class_name);
            MethodInfo* mi = ci ? symtab_find_method_in_class(ci, mname) : NULL;
            if (mi) {
                param_count = mi->param_count;
                param_defaults = mi->param_defaults;
            }
        } else if (ot->type_kind == TYPE_STRUCT) {
            StructInfo* si = symtab_find_struct(ot->class_name);
            MethodInfo* mi = si ? symtab_find_struct_method(si, mname) : NULL;
            if (mi) {
                param_count = mi->param_count;
                param_defaults = mi->param_defaults;
            }
        } else if (ot->type_kind == TYPE_INTERFACE) {
            InterfaceInfo* ii = symtab_find_interface(ot->class_name);
            InterfaceMethodInfo* im = ii ? symtab_find_interface_method(ii, mname) : NULL;
            if (im) {
                param_count = im->param_count;
                param_defaults = im->param_defaults;
            }
        }
        }
    } else if (callee->ast_kind == AST_IDENT) {
        FuncInfo* fi = symtab_find_func(callee->ast_token.text);
        if (fi && !fi->is_builtin) {
            param_count = fi->param_count;
            param_defaults = fi->param_defaults;
        }
    }
    if (!param_defaults) return;

    int actual = 0;
    AstNode* tail = NULL;
    if (node->ast_child_count > 1) {
        AstNode* a = node->ast_children[1];
        while (a) {
            tail = a;
            actual++;
            a = a->next;
        }
    }
    if (actual >= param_count) return;
    int i;
    for (i = actual; i < param_count; i++) {
        AstNode* def = param_defaults[i];
        /* Missing required parameter: reported by codegen_check_call_arity. */
        if (!def) break;
        /* Each call site gets its own clone so guard temporaries (assigned
           per call site) never alias the shared signature node. */
        AstNode* clone = ast_clone(def);
        clone->xor_str_id = def->xor_str_id;
        if (tail) {
            tail->next = clone;
        } else {
            node->ast_children[1] = clone;
            node->ast_child_count = 2;
        }
        tail = clone;
    }
}

/* Fills default arguments for every user call in an expression tree.  Runs
   before the f-string/subexpression/guard lowering passes so filled string
   literals get a guarded temporary and cleanup entry like written ones. */
static void materialize_call_defaults_walk(AstNode* node) {
    if (!node) return;
    if (node->ast_kind == AST_CALL) materialize_call_defaults(node);
    int i;
    for (i = 0; i < node->ast_child_count; i++) {
        materialize_call_defaults_walk(node->ast_children[i]);
    }
    materialize_call_defaults_walk(node->next);
}

/* Validates the argument count of a user call against its signature and
   reports an error on mismatch.  Default values make trailing parameters
   optional, so 'required' counts the leading parameters without one. */
static void codegen_check_call_arity(CodegenContext* ctx, int line, int col,
                                     const char* display_name, int actual,
                                     int param_count, AstNode* const* param_defaults) {
    int required = 0;
    while (required < param_count && !param_defaults[required]) required++;
    if (actual < required) {
        codegen_report_error(ctx, line, col,
                             "too few arguments for '%s' (expected at least %d, got %d)",
                             display_name, required, actual);
    } else if (actual > param_count) {
        codegen_report_error(ctx, line, col,
                             "too many arguments for '%s' (expected %d, got %d)",
                             display_name, param_count, actual);
    }
}

static void emit_array_ref_arg(CodegenContext* ctx, AstNode* arg) {
    if (!arg || arg->ast_kind != AST_IDENT) {
        codegen_report_error(ctx, arg ? arg->ast_token.line : 0, arg ? arg->ast_token.col : 0, "array move/copy destination must be a local variable");
        fprintf(ctx->out, "/* invalid array destination */");
        return;
    }
    SymEntry* e = symtab_lookup(arg->ast_token.text);
    if (!e) {
        codegen_report_error(ctx, arg->ast_token.line, arg->ast_token.col, "array move/copy destination must be a local variable");
        fprintf(ctx->out, "/* invalid array destination */");
        return;
    }
    if (type_is_ref(&e->type)) {
        fprintf(ctx->out, "%s", arg->ast_token.text);
    } else {
        fprintf(ctx->out, "&%s", arg->ast_token.text);
    }
}

static void codegen_array_method_call(CodegenContext* ctx, AstNode* arr,
                                      const char* mname, AstNode* args) {
    Type arr_type = arr->ast_resolved_type;
    char elem_size[128];
    char elem_type[128];
    array_elem_size_expr(&arr_type, elem_size, sizeof(elem_size));
    c_array_elem_type_name(&arr_type, elem_type, sizeof(elem_type));
    int kind = array_elem_kind(&arr_type);

    if (strcmp(mname, "length") == 0 || strcmp(mname, "capacity") == 0) {
        /* Value-returning reads of the MyArray struct members. */
        fprintf(ctx->out, "(");
        codegen_expr(ctx, arr);
        fprintf(ctx->out, ").%s", mname);
    } else if (strcmp(mname, "push") == 0) {
        if (!args) {
            codegen_report_error(ctx, arr->ast_token.line, arr->ast_token.col, "push() requires a value argument");
            fprintf(ctx->out, "0 /* missing push value */");
            return;
        }
        /* object arrays take interface values through their .data pointer
           and reject non-reference values. */
        int obj_iface_arg = 0;
        Type et = array_elem_type(&arr_type);
        if (et.type_kind == TYPE_OBJECT) {
            resolve_type(args);
            Type at = args->ast_resolved_type;
            if (at.type_kind == TYPE_INTERFACE && !at.is_weak) {
                obj_iface_arg = 1;
            } else if (at.type_kind != TYPE_CLASS && at.type_kind != TYPE_OBJECT &&
                       !type_is_null(&at)) {
                codegen_report_error(ctx, args->ast_token.line, args->ast_token.col, "cannot push '%s' to an 'object' array", type_name(&at));
            }
        }
        fprintf(ctx->out, "mylang_array_push(");
        emit_array_ptr_expr(ctx, arr);
        fprintf(ctx->out, ", %s, %d, (%s[]){", elem_size, kind, elem_type);
        if (obj_iface_arg) {
            fprintf(ctx->out, "(void*)(");
            codegen_expr(ctx, args);
            fprintf(ctx->out, ").data");
        } else {
            codegen_expr(ctx, args);
        }
        fprintf(ctx->out, "})");
    } else if (strcmp(mname, "pop") == 0) {
        fprintf(ctx->out, "mylang_array_pop(");
        emit_array_ptr_expr(ctx, arr);
        fprintf(ctx->out, ", %s, %d)", elem_size, kind);
    } else if (strcmp(mname, "reserve") == 0) {
        if (!args) {
            codegen_report_error(ctx, arr->ast_token.line, arr->ast_token.col, "reserve() requires a capacity argument");
            fprintf(ctx->out, "0 /* missing reserve capacity */");
            return;
        }
        fprintf(ctx->out, "mylang_array_reserve(");
        emit_array_ptr_expr(ctx, arr);
        fprintf(ctx->out, ", (size_t)(");
        codegen_expr(ctx, args);
        fprintf(ctx->out, "), %s)", elem_size);
    } else if (strcmp(mname, "resize") == 0) {
        if (!args) {
            codegen_report_error(ctx, arr->ast_token.line, arr->ast_token.col, "resize() requires a length argument");
            fprintf(ctx->out, "0 /* missing resize length */");
            return;
        }
        fprintf(ctx->out, "mylang_array_resize(");
        emit_array_ptr_expr(ctx, arr);
        fprintf(ctx->out, ", (size_t)(");
        codegen_expr(ctx, args);
        fprintf(ctx->out, "), %s, %d)", elem_size, kind);
    } else if (strcmp(mname, "clear") == 0) {
        fprintf(ctx->out, "mylang_array_clear(");
        emit_array_ptr_expr(ctx, arr);
        fprintf(ctx->out, ", %s, %d)", elem_size, kind);
    } else if (strcmp(mname, "compact") == 0) {
        fprintf(ctx->out, "mylang_array_compact(");
        emit_array_ptr_expr(ctx, arr);
        fprintf(ctx->out, ", %s)", elem_size);
    } else if (strcmp(mname, "move_to") == 0) {
        if (!args || args->ast_kind != AST_REF_ARG) {
            codegen_report_error(ctx, arr->ast_token.line, arr->ast_token.col, "move_to() requires a ref destination argument");
            fprintf(ctx->out, "0 /* missing move_to destination */");
            return;
        }
        fprintf(ctx->out, "mylang_array_move(");
        emit_array_ptr_expr(ctx, arr);
        fprintf(ctx->out, ", ");
        emit_array_ref_arg(ctx, args->ast_children[0]);
        fprintf(ctx->out, ", %s, %d)", elem_size, kind);
    } else if (strcmp(mname, "copy_to") == 0) {
        if (!args || args->ast_kind != AST_REF_ARG) {
            codegen_report_error(ctx, arr->ast_token.line, arr->ast_token.col, "copy_to() requires a ref destination argument");
            fprintf(ctx->out, "0 /* missing copy_to destination */");
            return;
        }
        fprintf(ctx->out, "mylang_array_copy(");
        emit_array_ptr_expr(ctx, arr);
        fprintf(ctx->out, ", ");
        emit_array_ref_arg(ctx, args->ast_children[0]);
        fprintf(ctx->out, ", %s, %d)", elem_size, kind);
    } else {
        codegen_report_error(ctx, arr->ast_token.line, arr->ast_token.col, "unknown array method '%s'", mname);
        fprintf(ctx->out, "0 /* unknown array method */");
    }
}

static void codegen_call(CodegenContext* ctx, AstNode* node) {
    /* Safety net: fill default arguments for call nodes that did not pass
       through prepare_expression (all statement expressions do). */
    materialize_call_defaults(node);
    /* method call: p.foo(...) ClassName_foo(p, ...) */
    if (node->ast_children[0]->ast_kind == AST_MEMBER_ACCESS) {
        AstNode* mem = node->ast_children[0];
        AstNode* obj = mem->ast_children[0];
        const char* mname = mem->ast_token.text;

        /* Static method call via the class name: ClassName.m(args). */
        {
            ClassInfo* sci = NULL;
            MethodInfo* smi = static_call_method(mem, &sci);
            if (sci) {
                if (!smi) {
                    codegen_report_error(ctx, mem->ast_token.line, mem->ast_token.col, "method '%s.%s' does not exist", sci->name, mname);
                    fprintf(ctx->out, "0 /* unknown static method */");
                    return;
                }
                if (!smi->method_is_static) {
                    codegen_report_error(ctx, mem->ast_token.line, mem->ast_token.col, "cannot call instance method '%s.%s' via the class name; use an instance", sci->name, mname);
                    fprintf(ctx->out, "0 /* instance method via class name */");
                    return;
                }
                if (!member_visible(ctx, sci->name, smi->is_private)) {
                    codegen_report_error(ctx, mem->ast_token.line, mem->ast_token.col, "cannot call private method '%s.%s'", sci->name, mname);
                }
                {
                    char dbuf[160];
                    int dn = snprintf(dbuf, sizeof(dbuf), "%s.%s", sci->name, mname);
                    CHECK_SNPRINTF(dn, sizeof(dbuf), "method display name too long");
                    codegen_check_call_arity(ctx, mem->ast_token.line, mem->ast_token.col,
                                             dbuf, count_call_args(node),
                                             smi->param_count, smi->param_defaults);
                }
                AstNode* sargs = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
                fprintf(ctx->out, "%s_%s(", class_c_name(sci), mname);
                int sidx = 0;
                while (sargs) {
                    if (sidx > 0) fprintf(ctx->out, ", ");
                    Type expected;
                    memset(&expected, 0, sizeof(expected));
                    if (sidx < smi->param_count) expected = smi->param_types[sidx];
                    codegen_call_arg(ctx, sargs, &expected);
                    sidx++;
                    sargs = sargs->next;
                }
                fprintf(ctx->out, ")");
                return;
            }
        }

        resolve_type(obj);
        AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;

        if (obj->ast_resolved_type.is_array && is_array_method_name(mname)) {
            if (strcmp(mname, "length") == 0 || strcmp(mname, "capacity") == 0) {
                codegen_check_call_arity(ctx, mem->ast_token.line, mem->ast_token.col,
                                         mname, count_call_args(node), 0, NULL);
            }
            codegen_array_method_call(ctx, obj, mname, args);
            return;
        }

        if (strcmp(mname, "lock") == 0 && obj->ast_resolved_type.is_weak) {
            if (obj->ast_resolved_type.type_kind == TYPE_INTERFACE) {
                fprintf(ctx->out, "mylang_lock_%s(", obj->ast_resolved_type.class_name);
                codegen_expr(ctx, obj);
                fprintf(ctx->out, ".wr, ");
                codegen_expr(ctx, obj);
                fprintf(ctx->out, ".vt)");
            } else {
                fprintf(ctx->out, "mylang_lock(");
                codegen_expr(ctx, obj);
                fprintf(ctx->out, ")");
            }
            return;
        }
        if (strcmp(mname, "lock") == 0 && obj->ast_resolved_type.is_unowned) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "unowned references do not have lock(); use them directly");
            fprintf(ctx->out, "0 /* invalid lock() on unowned */");
            return;
        }
        if (obj->ast_resolved_type.type_kind == TYPE_CLASS) {
            ClassInfo* ci = NULL;
            if (obj->ast_resolved_type.type_arg_count > 0) {
                ci = symtab_instantiate_class_from_type(&obj->ast_resolved_type);
            } else {
                ci = symtab_find_class(obj->ast_resolved_type.class_name);
            }
            MethodInfo* mi = symtab_find_method_in_class(ci, mname);
            if (ci && !mi) {
                codegen_report_error(ctx, mem->ast_token.line, mem->ast_token.col, "method '%s.%s' does not exist", ci->name, mname);
            }
            if (mi && mi->method_is_static) {
                codegen_report_error(ctx, mem->ast_token.line, mem->ast_token.col, "cannot call static method '%s.%s' via an instance; use the class name", ci->name, mname);
            }
            if (mi && ci && !member_visible(ctx, ci->name, mi->is_private)) {
                codegen_report_error(ctx, mem->ast_token.line, mem->ast_token.col, "cannot call private method '%s.%s'", ci->name, mname);
            }
            if (mi && ci) {
                char dbuf[160];
                int dn = snprintf(dbuf, sizeof(dbuf), "%s.%s", ci->name, mname);
                CHECK_SNPRINTF(dn, sizeof(dbuf), "method display name too long");
                codegen_check_call_arity(ctx, mem->ast_token.line, mem->ast_token.col,
                                         dbuf, count_call_args(node),
                                         mi->param_count, mi->param_defaults);
            }
            const char* class_c = ci ? class_c_name(ci) : obj->ast_resolved_type.class_name;
            fprintf(ctx->out, "%s_%s(", class_c, mname);
            codegen_expr(ctx, obj);
            int idx = 0;
            while (args) {
                fprintf(ctx->out, ", ");
                Type expected;
                memset(&expected, 0, sizeof(expected));
                if (mi && idx < mi->param_count) expected = mi->param_types[idx];
                codegen_call_arg(ctx, args, &expected);
                idx++;
                args = args->next;
            }
            fprintf(ctx->out, ")");
            return;
        }
        if (obj->ast_resolved_type.type_kind == TYPE_STRUCT) {
            /* Struct method call: StructName_method(&recv, args...).  The
               receiver must be an lvalue; '&' of the emitted expression works
               for locals (v -> &v), ref params ((*c) -> &(*c)) and array
               elements ((*p) -> &(*p)) alike. */
            StructInfo* si = symtab_find_struct(obj->ast_resolved_type.class_name);
            MethodInfo* mi = si ? symtab_find_struct_method(si, mname) : NULL;
            if (si && !mi) {
                codegen_report_error(ctx, mem->ast_token.line, mem->ast_token.col, "method '%s.%s' does not exist", si->name, mname);
            }
            if (mi && si) {
                char dbuf[160];
                int dn = snprintf(dbuf, sizeof(dbuf), "%s.%s", si->name, mname);
                CHECK_SNPRINTF(dn, sizeof(dbuf), "method display name too long");
                codegen_check_call_arity(ctx, mem->ast_token.line, mem->ast_token.col,
                                         dbuf, count_call_args(node),
                                         mi->param_count, mi->param_defaults);
            }
            if (obj->ast_kind != AST_IDENT && obj->ast_kind != AST_MEMBER_ACCESS &&
                obj->ast_kind != AST_ARRAY_ACCESS) {
                codegen_report_error(ctx, obj->ast_token.line, obj->ast_token.col, "struct method receiver must be a variable, field, or array element");
            }
            fprintf(ctx->out, "%s_%s(&(", si ? si->name : obj->ast_resolved_type.class_name, mname);
            codegen_expr(ctx, obj);
            fprintf(ctx->out, ")");
            int idx = 0;
            while (args) {
                fprintf(ctx->out, ", ");
                Type expected;
                memset(&expected, 0, sizeof(expected));
                if (mi && idx < mi->param_count) expected = mi->param_types[idx];
                codegen_call_arg(ctx, args, &expected);
                idx++;
                args = args->next;
            }
            fprintf(ctx->out, ")");
            return;
        }
        if (obj->ast_resolved_type.type_kind == TYPE_INTERFACE) {
            InterfaceInfo* ii = symtab_find_interface(obj->ast_resolved_type.class_name);
            InterfaceMethodInfo* im = NULL;
            if (ii) im = symtab_find_interface_method(ii, mname);
            if (ii && !im) {
                codegen_report_error(ctx, mem->ast_token.line, mem->ast_token.col, "method '%s.%s' does not exist", ii->name, mname);
            }
            if (im && ii) {
                char dbuf[160];
                int dn = snprintf(dbuf, sizeof(dbuf), "%s.%s", ii->name, mname);
                CHECK_SNPRINTF(dn, sizeof(dbuf), "method display name too long");
                codegen_check_call_arity(ctx, mem->ast_token.line, mem->ast_token.col,
                                         dbuf, count_call_args(node),
                                         im->param_count, im->param_defaults);
            }

            fprintf(ctx->out, "(");
            codegen_expr(ctx, obj);
            fprintf(ctx->out, ").vtable->%s((", mname);
            codegen_expr(ctx, obj);
            fprintf(ctx->out, ").data");

            int idx = 0;
            while (args) {
                fprintf(ctx->out, ", ");
                Type expected;
                memset(&expected, 0, sizeof(expected));
                if (im && idx < im->param_count) expected = im->param_types[idx];
                codegen_call_arg(ctx, args, &expected);
                idx++;
                args = args->next;
            }
            fprintf(ctx->out, ")");
            return;
        }
    }
    /* normal function call */
    AstNode* callee = node->ast_children[0];
    FuncInfo* fi = NULL;
    if (callee->ast_kind == AST_IDENT) {
        fi = symtab_find_func(callee->ast_token.text);
    }
    /* Builtin assert(cond): not registered in symtab (the condition accepts any
       truthy expression).  A user-defined function named 'assert' shadows it. */
    if (callee->ast_kind == AST_IDENT && !fi &&
        strcmp(callee->ast_token.text, "assert") == 0) {
        AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
        if (!args || args->next) {
            codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                 "assert expects 1 argument");
            fprintf(ctx->out, "(void)0");
            return;
        }
        fprintf(ctx->out, "((");
        codegen_expr(ctx, args);
        fprintf(ctx->out, ") ? (void)0 : mylang_assert_failed(%d, \"assertion failed\"))",
                callee->ast_token.line);
        return;
    }
    /* Builtin hash(x): like assert, not registered in symtab; the helper is
       picked from the argument's type.  A user-defined function named 'hash'
       shadows it. */
    if (callee->ast_kind == AST_IDENT && !fi &&
        strcmp(callee->ast_token.text, "hash") == 0) {
        AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
        if (!args || args->next) {
            codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                 "hash expects 1 argument");
            fprintf(ctx->out, "0ULL");
            return;
        }
        Type at = resolve_type(args);
        if (at.is_weak || at.is_unowned) {
            codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                 "cannot hash a weak/unowned reference; lock() it first");
            fprintf(ctx->out, "0ULL");
            return;
        }
        if (at.is_array || at.type_kind == TYPE_STRUCT) {
            codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                 "cannot hash values of this type");
            fprintf(ctx->out, "0ULL");
            return;
        }
        /* A class implementing IHashable hashes through its own hash(). */
        if (at.type_kind == TYPE_CLASS) {
            ClassInfo* ci = class_info_for_type(&at);
            if (class_implements(ci, "IHashable")) {
                MethodInfo* mi = symtab_find_method_in_class(ci, "hash");
                if (mi && mi->is_private) {
                    codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                         "hash() method of '%s' is private", ci->name);
                    fprintf(ctx->out, "0ULL");
                    return;
                }
                fprintf(ctx->out, "%s_hash(", class_c_prefix(ci));
                codegen_expr(ctx, args);
                fprintf(ctx->out, ")");
                return;
            }
        }
        if (at.type_kind == TYPE_BOOL || type_is_integer(&at)) {
            fprintf(ctx->out, "mylang_hash_u64((uint64_t)(");
            codegen_expr(ctx, args);
            fprintf(ctx->out, "))");
        } else if (at.type_kind == TYPE_F32 || at.type_kind == TYPE_F64) {
            fprintf(ctx->out, "mylang_hash_f64((double)(");
            codegen_expr(ctx, args);
            fprintf(ctx->out, "))");
        } else if (at.type_kind == TYPE_CLASS && strcmp(at.class_name, "String") == 0) {
            fprintf(ctx->out, "mylang_hash_string(");
            codegen_expr(ctx, args);
            fprintf(ctx->out, ")");
        } else if (at.type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "mylang_hash_ptr((");
            codegen_expr(ctx, args);
            fprintf(ctx->out, ").data)");
        } else {
            /* Other class types, object, and null: identity hash. */
            fprintf(ctx->out, "mylang_hash_ptr(");
            codegen_expr(ctx, args);
            fprintf(ctx->out, ")");
        }
        return;
    }
    /* Builtin equals(a, b): dispatches on the first argument's type, mirroring
       hash(x).  A user-defined function named 'equals' shadows it. */
    if (callee->ast_kind == AST_IDENT && !fi &&
        strcmp(callee->ast_token.text, "equals") == 0) {
        AstNode* a = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
        AstNode* b = a ? a->next : NULL;
        if (!a || !b || b->next) {
            codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                 "equals expects 2 arguments");
            fprintf(ctx->out, "0");
            return;
        }
        Type at = resolve_type(a);
        Type bt = resolve_type(b);
        if (at.is_weak || at.is_unowned || bt.is_weak || bt.is_unowned) {
            codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                 "cannot compare weak/unowned references with equals; lock() them first");
            fprintf(ctx->out, "0");
            return;
        }
        if (at.is_array || bt.is_array || at.type_kind == TYPE_STRUCT || bt.type_kind == TYPE_STRUCT) {
            codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                 "cannot compare values of this type with equals");
            fprintf(ctx->out, "0");
            return;
        }
        int a_ref = type_is_reference(&at) || type_is_null(&at);
        int b_ref = type_is_reference(&bt) || type_is_null(&bt);
        if (a_ref != b_ref) {
            codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                 "cannot compare '%s' with '%s'", type_name(&at), type_name(&bt));
            fprintf(ctx->out, "0");
            return;
        }
        if (!a_ref) {
            /* bool, integer, and float values compare directly. */
            fprintf(ctx->out, "(");
            codegen_expr(ctx, a);
            fprintf(ctx->out, " == ");
            codegen_expr(ctx, b);
            fprintf(ctx->out, ")");
            return;
        }
        if (type_is_null(&at) && type_is_null(&bt)) {
            fprintf(ctx->out, "1");
            return;
        }
        if (at.type_kind == TYPE_CLASS && strcmp(at.class_name, "String") == 0) {
            if (!type_is_null(&bt) &&
                !(bt.type_kind == TYPE_CLASS && strcmp(bt.class_name, "String") == 0)) {
                codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                     "cannot compare 'string' with '%s'", type_name(&bt));
                fprintf(ctx->out, "0");
                return;
            }
            fprintf(ctx->out, "String_equals(");
            codegen_expr(ctx, a);
            fprintf(ctx->out, ", ");
            codegen_expr(ctx, b);
            fprintf(ctx->out, ")");
            return;
        }
        if (at.type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "(");
            codegen_expr(ctx, a);
            fprintf(ctx->out, ").data == ");
            if (bt.type_kind == TYPE_INTERFACE) {
                fprintf(ctx->out, "(");
                codegen_expr(ctx, b);
                fprintf(ctx->out, ").data)");
            } else {
                codegen_expr(ctx, b);
                fprintf(ctx->out, ")");
            }
            return;
        }
        if (at.type_kind == TYPE_CLASS && !type_is_null(&at)) {
            ClassInfo* ci = class_info_for_type(&at);
            if (class_implements(ci, "IHashable")) {
                MethodInfo* mi = symtab_find_method_in_class(ci, "equals");
                if (mi && mi->is_private) {
                    codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col,
                                         "equals(object) method of '%s' is private", ci->name);
                    fprintf(ctx->out, "0");
                    return;
                }
                fprintf(ctx->out, "%s_equals(", class_c_prefix(ci));
                codegen_expr(ctx, a);
                fprintf(ctx->out, ", ");
                codegen_expr(ctx, b);
                fprintf(ctx->out, ")");
                return;
            }
        }
        /* object, other class types, and null: identity comparison. */
        fprintf(ctx->out, "(");
        codegen_expr(ctx, a);
        fprintf(ctx->out, " == ");
        codegen_expr(ctx, b);
        fprintf(ctx->out, ")");
        return;
    }
    if (fi && fi->is_builtin) {
        if (strcmp(fi->name, "print") == 0) {
            AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
            fprintf(ctx->out, "mylang_print_string(");
            if (args) {
                Type expected;
                memset(&expected, 0, sizeof(expected));
                if (fi->param_count > 0) expected = fi->param_types[0];
                codegen_call_arg(ctx, args, &expected);
            } else {
                fprintf(ctx->out, "NULL");
            }
            fprintf(ctx->out, ")");
            return;
        }
    }
    if (callee->ast_kind == AST_MEMBER_ACCESS) {
        codegen_expr(ctx, callee);
    } else if (callee->ast_kind == AST_IDENT) {
        if (!fi) {
            codegen_report_error(ctx, callee->ast_token.line, callee->ast_token.col, "unknown function '%s'", callee->ast_token.text);
        }
        fprintf(ctx->out, "%s", callee->ast_token.text);
    } else {
        codegen_expr(ctx, callee);
    }
    fprintf(ctx->out, "(");
    AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
    if (fi && !fi->is_builtin) {
        codegen_check_call_arity(ctx, callee->ast_token.line, callee->ast_token.col,
                                 callee->ast_token.text, count_call_args(node),
                                 fi->param_count, fi->param_defaults);
    }
    int first = 1;
    int idx = 0;
    while (args) {
        if (!first) fprintf(ctx->out, ", ");
        Type expected;
        memset(&expected, 0, sizeof(expected));
        if (fi && idx < fi->param_count) expected = fi->param_types[idx];
        codegen_call_arg(ctx, args, &expected);
        first = 0;
        idx++;
        args = args->next;
    }
    fprintf(ctx->out, ")");
}

static void emit_array_ptr_expr(CodegenContext* ctx, AstNode* arr_node) {
    /* codegen_expr yields the MyArray value; take its address for helpers. */
    fprintf(ctx->out, "&(");
    codegen_expr(ctx, arr_node);
    fprintf(ctx->out, ")");
}

static void codegen_array_access(CodegenContext* ctx, AstNode* node) {
    AstNode* arr = node->ast_children[0];
    AstNode* idx = node->ast_children[1];
    char elem_type[128];
    char elem_size[128];
    resolve_type(arr);
    if (type_is_null(&arr->ast_resolved_type)) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot index null");
        fprintf(ctx->out, "0 /* invalid null index */");
        return;
    }
    (void)resolve_type(node);
    c_array_elem_type_name(&arr->ast_resolved_type, elem_type, sizeof(elem_type));
    array_elem_size_expr(&arr->ast_resolved_type, elem_size, sizeof(elem_size));
    fprintf(ctx->out, "(*(%s*)mylang_array_at(", elem_type);
    emit_array_ptr_expr(ctx, arr);
    fprintf(ctx->out, ", ");
    codegen_expr(ctx, idx);
    fprintf(ctx->out, ", %s))", elem_size);
}

/* If node is a member access that resolves to a class property, returns the
   PropertyInfo and optionally the owning class.  Used by the prepare-time
   property lowering, the expr_is_owned safety net, and the property read,
   write, and increment/decrement dispatch paths. */
static PropertyInfo* member_access_property(AstNode* node, ClassInfo** out_ci) {
    if (!node || node->ast_kind != AST_MEMBER_ACCESS) return NULL;
    AstNode* obj = node->ast_children[0];
    if (!obj) return NULL;
    resolve_type(obj);
    if (obj->ast_resolved_type.type_kind != TYPE_CLASS) return NULL;
    ClassInfo* ci = symtab_find_class(obj->ast_resolved_type.class_name);
    if (!ci || ci->is_generic) return NULL;
    PropertyInfo* pi = symtab_find_property(ci, node->ast_token.text);
    if (pi && out_ci) *out_ci = ci;
    return pi;
}

/* Turn a property access node into an ordinary accessor call node:
   the member access obj.X becomes the call obj.get_X() (args == NULL) or,
   for an assignment node obj.X = rhs, the call obj.set_X(rhs).  The object
   node moves under the synthetic callee; the old node is rewritten in place
   so parents and list links stay valid. */
static void property_to_call(AstNode* node, AstNode* obj, AstNode* args,
                             const char* prefix, const char* prop_name) {
    Token tok = node->ast_token;
    int n = snprintf(tok.text, sizeof(tok.text), "%s_%s", prefix, prop_name);
    CHECK_SNPRINTF(n, sizeof(tok.text), "accessor name too long");
    AstNode* callee = ast_new_node(AST_MEMBER_ACCESS, tok);
    ast_add_child(callee, obj);
    memset(node->ast_children, 0, sizeof(node->ast_children));
    node->ast_kind = AST_CALL;
    node->ast_token = tok;
    node->ast_child_count = 0;
    ast_add_child(node, callee);
    if (args) {
        args->next = NULL;
        ast_add_child(node, args);
    }
    memset(&node->ast_resolved_type, 0, sizeof(node->ast_resolved_type));
    node->ast_is_resolved = 0;  /* node rewritten: invalidate the sema cache */
    node->ast_temp_name[0] = '\0';
    node->ast_match_var[0] = '\0';
}

/* Lower valid property accesses into accessor calls before the temporary,
   guard, and f-string passes run, so reference-typed properties get the same
   ownership treatment as method calls:
     read   obj.X       -> obj.get_X()
     write  obj.X = rhs -> obj.set_X(rhs)
   Invalid accesses (no accessor, private, compound assignment, ++/--) are
   left untouched for the dispatch paths, which report the diagnostics. */
static void lower_property_access(CodegenContext* ctx, AstNode* node) {
    if (!node) return;
    if (node->ast_kind == AST_ASSIGN) {
        AstNode* lhs = node->ast_children[0];
        AstNode* rhs = node->ast_children[1];
        ClassInfo* pci = NULL;
        PropertyInfo* pi = NULL;
        if (!is_compound_assign_op(node->ast_token.kind)) {
            pi = member_access_property(lhs, &pci);
        }
        if (pi && pi->has_set && member_visible(ctx, pci->name, pi->is_private)) {
            property_to_call(node, lhs->ast_children[0], rhs, "set", pi->name);
            lower_property_access(ctx, node->ast_children[0]->ast_children[0]);
            lower_property_access(ctx, node->ast_children[1]);
        } else {
            /* Keep the LHS shape for the dispatch diagnostics; still lower
               property reads inside the object expression. */
            if (lhs && lhs->ast_kind == AST_MEMBER_ACCESS) {
                lower_property_access(ctx, lhs->ast_children[0]);
            } else {
                lower_property_access(ctx, lhs);
            }
            lower_property_access(ctx, rhs);
        }
        lower_property_access(ctx, node->next);
        return;
    }
    if (node->ast_kind == AST_INC_DEC) {
        AstNode* operand = node->ast_children[0];
        if (operand && operand->ast_kind == AST_MEMBER_ACCESS) {
            lower_property_access(ctx, operand->ast_children[0]);
        } else {
            lower_property_access(ctx, operand);
        }
        lower_property_access(ctx, node->next);
        return;
    }
    if (node->ast_kind == AST_MEMBER_ACCESS) {
        ClassInfo* pci = NULL;
        PropertyInfo* pi = member_access_property(node, &pci);
        if (pi && pi->has_get && member_visible(ctx, pci->name, pi->is_private)) {
            property_to_call(node, node->ast_children[0], NULL, "get", pi->name);
            lower_property_access(ctx, node->ast_children[0]->ast_children[0]);
        } else {
            lower_property_access(ctx, node->ast_children[0]);
        }
        lower_property_access(ctx, node->next);
        return;
    }
    {
        int i;
        for (i = 0; i < node->ast_child_count; i++) {
            AstNode* child = node->ast_children[i];
            if (node->ast_kind == AST_CALL && i == 0 &&
                child && child->ast_kind == AST_MEMBER_ACCESS) {
                /* The callee of a call is not a read position: obj.X() where
                   X names a property is reported by codegen_call. */
                lower_property_access(ctx, child->ast_children[0]);
            } else {
                lower_property_access(ctx, child);
            }
        }
    }
    lower_property_access(ctx, node->next);
}

static void codegen_member_access(CodegenContext* ctx, AstNode* node) {
    AstNode* obj = node->ast_children[0];
    /* Enum variant access 'Key.Up' emits the C constant 'Key_Up'.  A local
       variable of the same name shadows the enum (same rule as class
       static calls in static_call_method). */
    if (obj->ast_kind == AST_IDENT && !symtab_lookup(obj->ast_token.text)) {
        EnumInfo* ei = symtab_find_enum(obj->ast_token.text);
        if (ei) {
            int found = 0;
            int i;
            for (i = 0; i < ei->variant_count; i++) {
                if (strcmp(ei->variant_names[i], node->ast_token.text) == 0) { found = 1; break; }
            }
            if (!found) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                                     "enum '%s' has no variant '%s'", ei->name, node->ast_token.text);
                fprintf(ctx->out, "0 /* invalid enum variant */");
                return;
            }
            fprintf(ctx->out, "%s_%s", ei->name, node->ast_token.text);
            return;
        }
    }
    /* Static class const access 'Config.MAX' emits the C constant
       'Config_MAX'.  A local variable of the same name shadows the class
       (same rule as static calls). */
    if (obj->ast_kind == AST_IDENT && !symtab_lookup(obj->ast_token.text)) {
        ClassInfo* ci = symtab_find_class(obj->ast_token.text);
        if (ci && !ci->is_generic) {
            ConstInfo* cc = symtab_find_class_const(ci->name, node->ast_token.text);
            if (cc) {
                if (!member_visible(ctx, ci->name, cc->is_private)) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                                         "static const '%s.%s' is private", ci->name, cc->name);
                    fprintf(ctx->out, "0 /* private static const */");
                    return;
                }
                char cname[2 * NAME_BUF_SIZE];
                const_c_name(cc, cname, sizeof(cname));
                fprintf(ctx->out, "%s", cname);
                return;
            }
        }
    }
    resolve_type(obj);
    if (type_is_null(&obj->ast_resolved_type)) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot access member '%s' on null", node->ast_token.text);
        fprintf(ctx->out, "0 /* invalid null member access */");
        return;
    }
    if (obj->ast_resolved_type.type_kind == TYPE_OBJECT && !obj->ast_resolved_type.is_array) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot access member '%s' on object; cast it with 'as' first", node->ast_token.text);
        fprintf(ctx->out, "0 /* invalid object member access */");
        return;
    }
    if (obj->ast_resolved_type.is_array) {
        /* Arrays have no member fields; all operations (length(), capacity(),
           push(), ...) are builtin methods dispatched as calls. */
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                             "array has no member '%s'", node->ast_token.text);
        fprintf(ctx->out, "0 /* invalid array member access */");
        return;
    } else if (obj->ast_resolved_type.type_kind == TYPE_CLASS) {
        /* Class references may come from void* getters (e.g. dynamic arrays),
           so cast to the concrete struct pointer before using ->.  An unowned
           reference is checked first by the expression wrapper, so the cast
           target here is the concrete class, not WeakRef. */
        Type ct = obj->ast_resolved_type;
        ct.is_unowned = 0;

        /* Access control: a private field is visible only inside methods of
           the same class.  An unknown member name is an error here instead of
           falling through to the C compiler. */
        {
            Type ot = obj->ast_resolved_type;
            ClassInfo* ci = NULL;
            if (ot.type_arg_count > 0) {
                ci = symtab_instantiate_class_from_type(&ot);
            } else {
                ci = symtab_find_class(ot.class_name);
            }
            if (ci) {
                int found = 0;
                int i;
                for (i = 0; i < ci->field_count; i++) {
                    if (strcmp(ci->field_names[i], node->ast_token.text) == 0) {
                        found = 1;
                        if (!member_visible(ctx, ci->name, ci->field_private[i])) {
                            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot access private field '%s.%s'", ci->name, node->ast_token.text);
                        }
                        break;
                    }
                }
                if (!found) {
                    PropertyInfo* pi = symtab_find_property(ci, node->ast_token.text);
                    if (pi) {
                        /* Property read: obj.X lowers to the getter call
                           Class_get_X(obj). */
                        if (!pi->has_get) {
                            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                                                 "property '%s.%s' has no getter", ci->name, pi->name);
                        } else if (!member_visible(ctx, ci->name, pi->is_private)) {
                            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                                                 "cannot access private property '%s.%s'", ci->name, pi->name);
                        } else {
                            fprintf(ctx->out, "%s_get_%s(", ci->name, pi->name);
                            codegen_expr(ctx, obj);
                            fprintf(ctx->out, ")");
                        }
                        return;
                    }
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                                         "class '%s' has no field '%s'", ci->name, node->ast_token.text);
                }
            }
        }

        fprintf(ctx->out, "((%s*)", c_base_name(&ct));
        codegen_expr(ctx, obj);
        fprintf(ctx->out, ")->%s", node->ast_token.text);
    } else {
        /* Structs (by value or via a ref parameter): validate the field name
           so a typo is caught here instead of by the C compiler. */
        if (obj->ast_resolved_type.type_kind == TYPE_STRUCT) {
            StructInfo* si = symtab_find_struct(obj->ast_resolved_type.class_name);
            if (si) {
                int found = 0;
                int i;
                for (i = 0; i < si->field_count; i++) {
                    if (strcmp(si->field_names[i], node->ast_token.text) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                                         "struct '%s' has no field '%s'", si->name, node->ast_token.text);
                }
            }
        }
        if (obj->ast_resolved_type.is_pointer) {
            codegen_expr(ctx, obj);
            fprintf(ctx->out, "->%s", node->ast_token.text);
        } else {
            codegen_expr(ctx, obj);
            fprintf(ctx->out, ".%s", node->ast_token.text);
        }
    }
}

static void codegen_new(CodegenContext* ctx, AstNode* node) {
    Type base = node->ast_resolved_type;
    if (base.type_kind == TYPE_CLASS && base.type_arg_count > 0) {
        symtab_instantiate_class_from_type(&base);
    }
    if (node->ast_child_count > 0) {
        /* Parser rejects 'new T[N]'; arrays are created empty and grown with
           push/reserve.  This branch is only reached on parse-error recovery. */
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "'new %s[...]' is not supported; use '%s[] name; name.reserve(size)'", base.class_name, base.class_name);
        fprintf(ctx->out, "/* invalid new array */");
    } else {
        if (base.type_kind == TYPE_CLASS) {
            char dtor_name[128];
            int n;
            ClassInfo* ci = symtab_find_class(base.class_name);
            if (!ci) ci = symtab_find_class_by_mangled(base.class_name);
            if (ci && ci->is_generic && base.type_arg_count == 0) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                                     "generic class '%s' requires %d type argument(s)",
                                     base.class_name, ci->generic_param_count);
            }
            if (ci && ci->mangled_name[0]) {
                n = snprintf(dtor_name, sizeof(dtor_name), "_mylang_dtor_%s", ci->mangled_name);
            } else {
                n = snprintf(dtor_name, sizeof(dtor_name), "_mylang_dtor_%s", c_base_name(&base));
            }
            CHECK_SNPRINTF(n, sizeof(dtor_name), "destructor name too long");
            fprintf(ctx->out, "mylang_new_object(sizeof(%s), %u, %s)", c_base_name(&base), (unsigned)base.type_id, dtor_name);
        } else {
            fprintf(ctx->out, "calloc(1, sizeof(%s))", c_base_name(&base));
        }
    }
    node->ast_resolved_type = base;
}

static void codegen_char_lit(CodegenContext* ctx, AstNode* node) {
    char c = node->ast_token.char_val;
    switch (c) {
        case '\n': fprintf(ctx->out, "'\\n'"); break;
        case '\t': fprintf(ctx->out, "'\\t'"); break;
        case '\r': fprintf(ctx->out, "'\\r'"); break;
        case '\\': fprintf(ctx->out, "'\\\\'"); break;
        case '\'': fprintf(ctx->out, "'\\''"); break;
        default:   fprintf(ctx->out, "'%c'", c); break;
    }
}

static void emit_c_string_literal(CodegenContext* ctx, const char* s) {
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        switch (c) {
            case '\n': fprintf(ctx->out, "\\n"); break;
            case '\t': fprintf(ctx->out, "\\t"); break;
            case '\r': fprintf(ctx->out, "\\r"); break;
            case '\\': fprintf(ctx->out, "\\\\"); break;
            case '"':  fprintf(ctx->out, "\\\""); break;
            default:
                if (c >= 32 && c <= 126) {
                    fprintf(ctx->out, "%c", (char)c);
                } else {
                    fprintf(ctx->out, "\\x%02X", c);
                }
                break;
        }
    }
}

static uint8_t xor_string_key(int id) {
    return (uint8_t)(1 + ((id - 1) % 255));
}

static void emit_xor_string_array_decl(CodegenContext* ctx, const char* s, int id) {
    size_t len = strlen(s);
    if (len == 0) return;
    uint8_t key = xor_string_key(id);
    fprintf(ctx->out, "static const uint8_t _xs%d[] = {", id);
    size_t i;
    for (i = 0; i < len; i++) {
        if (i > 0) fprintf(ctx->out, ",");
        fprintf(ctx->out, "0x%02X", (unsigned char)(s[i] ^ key));
    }
    fprintf(ctx->out, "}; // \"");
    emit_c_string_literal(ctx, s);
    fprintf(ctx->out, "\"\n");
}

static void codegen_collect_xor_strings(CodegenContext* ctx, AstNode* node) {
    if (!node) return;
    if (node->ast_kind == AST_STRING_LIT) {
        if (node->xor_str_id == 0 && strlen(node->ast_token.text) > 0) {
            node->xor_str_id = ++ctx->xor_str_id;
            emit_xor_string_array_decl(ctx, node->ast_token.text, node->xor_str_id);
        }
    }
    int i;
    for (i = 0; i < node->ast_child_count; i++) {
        codegen_collect_xor_strings(ctx, node->ast_children[i]);
    }
    codegen_collect_xor_strings(ctx, node->next);
}

/* Registers string literals stored as parameter default values with the
   string-encryption table.  Defaults live in the symbol table, outside the
   program AST, so codegen_collect_xor_strings never visits them; each
   call-site clone copies the id from the shared node. */
static void codegen_collect_xor_defaults(CodegenContext* ctx) {
    extern ClassInfo* class_list;
    extern InterfaceInfo* interface_list;
    int i;
    FuncInfo* fi;
    for (fi = symtab_first_func(); fi; fi = fi->next) {
        for (i = 0; i < fi->param_count && i < MAX_PARAMS; i++) {
            codegen_collect_xor_strings(ctx, fi->param_defaults[i]);
        }
    }
    ClassInfo* ci;
    for (ci = class_list; ci; ci = ci->next) {
        MethodInfo* mi;
        for (mi = ci->methods; mi; mi = mi->next) {
            for (i = 0; i < mi->param_count && i < MAX_PARAMS; i++) {
                codegen_collect_xor_strings(ctx, mi->param_defaults[i]);
            }
        }
    }
    StructInfo* si;
    for (si = symtab_first_struct(); si; si = si->next) {
        MethodInfo* mi;
        for (mi = si->methods; mi; mi = mi->next) {
            for (i = 0; i < mi->param_count && i < MAX_PARAMS; i++) {
                codegen_collect_xor_strings(ctx, mi->param_defaults[i]);
            }
        }
    }
    InterfaceInfo* ii;
    for (ii = interface_list; ii; ii = ii->next) {
        int j;
        for (j = 0; j < ii->method_count; j++) {
            for (i = 0; i < ii->methods[j].param_count && i < MAX_PARAMS; i++) {
                codegen_collect_xor_strings(ctx, ii->methods[j].param_defaults[i]);
            }
        }
    }
}

static void codegen_string_lit(CodegenContext* ctx, AstNode* node) {
    const char* s = node->ast_token.text;
    if (ctx->xor_strings) {
        size_t len = strlen(s);
        if (len == 0) {
            fprintf(ctx->out, "mylang_string_new_encrypted(MYLANG_TID_String, NULL, 0, 1)");
        } else {
            int id = node->xor_str_id;
            if (id == 0) {
                id = ++ctx->xor_str_id;
                node->xor_str_id = id;
                emit_xor_string_array_decl(ctx, s, id);
            }
            fprintf(ctx->out, "mylang_string_new_encrypted(MYLANG_TID_String, _xs%d, %zu, %u)",
                    id, len, (unsigned)xor_string_key(id));
        }
    } else {
        fprintf(ctx->out, "mylang_string_new(MYLANG_TID_String, \"");
        emit_c_string_literal(ctx, s);
        fprintf(ctx->out, "\")");
    }
}

/* Expression entry point.  Reading an unowned reference as a value goes
   through the runtime liveness check (my_panic on a dead object); weak and
   unowned share-management paths bypass the wrapper via codegen_expr_raw. */
static void codegen_expr(CodegenContext* ctx, AstNode* node) {
    if (!node) return;
    resolve_type(node);
    int wrap_unowned = node->ast_resolved_type.is_unowned &&
                       !ctx->no_unowned_check &&
                       (node->ast_kind == AST_IDENT ||
                        node->ast_kind == AST_MEMBER_ACCESS);
    if (wrap_unowned) fprintf(ctx->out, "mylang_unowned_check(");
    codegen_expr_dispatch(ctx, node);
    if (wrap_unowned) fprintf(ctx->out, ")");
}

/* Emit an expression without the unowned liveness-check wrapper.  Used where
   the raw WeakRef* value is required (weak/unowned copies and weakifying). */
static void codegen_expr_raw(CodegenContext* ctx, AstNode* node) {
    int saved = ctx->no_unowned_check;
    ctx->no_unowned_check = 1;
    codegen_expr(ctx, node);
    ctx->no_unowned_check = saved;
}

static void codegen_expr_dispatch(CodegenContext* ctx, AstNode* node) {
    if (!node) return;
    resolve_type(node);

    if (node->ast_temp_name[0] != '\0') {
        fprintf(ctx->out, "%s", node->ast_temp_name);
        return;
    }

    switch (node->ast_kind) {
        case AST_INT_LIT:
            fprintf(ctx->out, "%d", node->ast_token.int_val);
            break;
        case AST_BOOL_LIT:
            fprintf(ctx->out, "%d", node->ast_token.int_val);
            break;
        case AST_NULL:
            fprintf(ctx->out, "NULL");
            break;
        case AST_FLOAT_LIT:
            fprintf(ctx->out, "%s", node->ast_token.text);
            break;
        case AST_CHAR_LIT:
            codegen_char_lit(ctx, node);
            break;
        case AST_STRING_LIT:
            codegen_string_lit(ctx, node);
            break;
        case AST_IDENT: {
            if (strcmp(node->ast_token.text, "this") == 0) {
                if (ctx->current_method_is_static) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "'this' cannot be used in a static method");
                    fprintf(ctx->out, "0 /* this in static method */");
                    break;
                }
                if (ctx->is_interface_default_method) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "'this' is not allowed in interface default method");
                }
                /* In struct methods 'this' is a ref alias: thiz is a pointer,
                   so dereference it to produce the struct value. */
                SymEntry* te = symtab_lookup("this");
                if (te && type_is_ref(&te->type)) {
                    fprintf(ctx->out, "(*thiz)");
                } else {
                    fprintf(ctx->out, "thiz");
                }
            } else {
                SymEntry* e = symtab_lookup(node->ast_token.text);
                if (!e) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "unknown identifier '%s'", node->ast_token.text);
                    fprintf(ctx->out, "0 /* unknown identifier */");
                    break;
                }
                if (e && type_is_ref(&e->type)) {
                    fprintf(ctx->out, "(*%s)", node->ast_token.text);
                } else {
                    fprintf(ctx->out, "%s", node->ast_token.text);
                }
            }
            break;
        }
        case AST_BINARY:
            codegen_binary(ctx, node);
            break;
        case AST_UNARY:
            codegen_unary(ctx, node);
            break;
        case AST_INC_DEC: {
            AstNode* operand = node->ast_children[0];
            if (member_access_property(operand, NULL)) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "increment/decrement is not supported on properties");
                fprintf(ctx->out, "0 /* invalid property increment/decrement */");
                break;
            }
            resolve_type(operand);
            Type t = operand->ast_resolved_type;
            int is_primitive_numeric = (t.type_kind == TYPE_I8 || t.type_kind == TYPE_I16 ||
                                        t.type_kind == TYPE_I32 || t.type_kind == TYPE_I64 ||
                                        t.type_kind == TYPE_U8 || t.type_kind == TYPE_U16 ||
                                        t.type_kind == TYPE_U32 || t.type_kind == TYPE_U64 ||
                                        t.type_kind == TYPE_F32 || t.type_kind == TYPE_F64);
            if (!is_primitive_numeric) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "increment/decrement not supported for this type");
                fprintf(ctx->out, "0 /* invalid increment/decrement */");
                break;
            }
            if (t.is_const) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot modify const variable '%s'", operand->ast_token.text);
                fprintf(ctx->out, "0 /* invalid const increment/decrement */");
                break;
            }
            const char* op_text = (node->ast_token.kind == TOK_INC) ? "+" : "-";
            codegen_expr(ctx, operand);
            fprintf(ctx->out, " = ");
            codegen_expr(ctx, operand);
            fprintf(ctx->out, " %s 1", op_text);
            break;
        }
        case AST_ASSIGN: {
            AstNode* lhs = node->ast_children[0];
            AstNode* rhs = node->ast_children[1];
            resolve_type(lhs);
            Type lt = lhs->ast_resolved_type;

            if (lt.is_array) {
                codegen_report_error(ctx, lhs->ast_token.line, lhs->ast_token.col, "cannot assign arrays directly; use move_to(ref) or copy_to(ref)");
                fprintf(ctx->out, "0 /* invalid array assignment */");
                break;
            }

            if (type_is_null(&lt)) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign to null literal");
                fprintf(ctx->out, "0 /* invalid assignment target */");
                break;
            }

            if (lt.is_const) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign to const variable '%s'", lhs->ast_token.text);
                fprintf(ctx->out, "0 /* invalid const assignment */");
                break;
            }

            /* Property assignment: obj.X = rhs.  Valid property writes have
               already been rewritten into setter calls by
               lower_property_access during prepare; this block only remains
               as a fallback for expressions that bypass prepare, and to
               report the dedicated diagnostics for the cases lowering skips
               (compound assignment, no setter, private). */
            {
                ClassInfo* pci = NULL;
                PropertyInfo* pi = member_access_property(lhs, &pci);
                if (pi) {
                    if (is_compound_assign_op(node->ast_token.kind)) {
                        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "compound assignment is not supported on property '%s'", pi->name);
                        fprintf(ctx->out, "0 /* invalid property compound assignment */");
                        break;
                    }
                    if (!pi->has_set) {
                        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "property '%s.%s' has no setter", pci->name, pi->name);
                        fprintf(ctx->out, "0 /* invalid property assignment */");
                        break;
                    }
                    if (!member_visible(ctx, pci->name, pi->is_private)) {
                        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot access private property '%s.%s'", pci->name, pi->name);
                        fprintf(ctx->out, "0 /* private property assignment */");
                        break;
                    }
                    resolve_type(rhs);
                    Type rt = rhs->ast_resolved_type;
                    if (type_is_null(&rt) || type_is_reference(&rt) ||
                        bool_mismatch(&lt, &rt) || enum_mismatch(&lt, &rt)) {
                        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to property '%s'", type_name(&rt), type_name(&lt));
                        fprintf(ctx->out, "0 /* invalid property assignment */");
                        break;
                    }
                    fprintf(ctx->out, "%s_set_%s(", pci->name, pi->name);
                    codegen_expr(ctx, lhs->ast_children[0]);
                    fprintf(ctx->out, ", ");
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ")");
                    break;
                }
            }

            {
                TokenKind assign_op = node->ast_token.kind;
                if (is_compound_assign_op(assign_op)) {
                    /* Compound assignment: arithmetic ops accept primitive
                       numeric types; bitwise ops require integer types. */
                    int type_ok = is_bit_compound_op(assign_op)
                        ? type_is_integer(&lt) : type_is_numeric(&lt);
                    if (!type_ok) {
                        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "compound assignment not supported for this type");
                        fprintf(ctx->out, "0 /* invalid compound assignment */");
                        break;
                    }
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, " = ");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, " %s ", compound_op_text(assign_op));
                    codegen_expr(ctx, rhs);
                    break;
                }
            }

            if (lhs->ast_kind == AST_ARRAY_ACCESS) {
                Type at = lhs->ast_children[0]->ast_resolved_type;
                if (at.is_array) {
                    /* array element assignment */
                    int rhs_owned = (expr_is_owned(rhs));
                    resolve_type(rhs);
                    Type rt = rhs->ast_resolved_type;

                    if (lt.type_kind == TYPE_CLASS && !lt.is_weak) {
                        fprintf(ctx->out, "((void)mylang_release(");
                        codegen_array_access(ctx, lhs);
                        fprintf(ctx->out, "), ");
                        codegen_array_access(ctx, lhs);
                        fprintf(ctx->out, " = ");
                        if (rhs_owned) {
                            codegen_expr(ctx, rhs);
                        } else {
                            fprintf(ctx->out, "mylang_retain(");
                            codegen_expr(ctx, rhs);
                            fprintf(ctx->out, ")");
                        }
                        fprintf(ctx->out, ")");
                    } else if (lt.type_kind == TYPE_OBJECT) {
                        /* object array element: an interface RHS contributes
                           its .data pointer. */
                        int rhs_iface = (rt.type_kind == TYPE_INTERFACE && !rt.is_weak);
                        if (!rhs_iface && rt.type_kind != TYPE_CLASS &&
                            rt.type_kind != TYPE_OBJECT && !type_is_null(&rt)) {
                            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to 'object' array element", type_name(&rt));
                            fprintf(ctx->out, "0 /* invalid object element assignment */");
                        } else {
                            fprintf(ctx->out, "((void)mylang_release(");
                            codegen_array_access(ctx, lhs);
                            fprintf(ctx->out, "), ");
                            codegen_array_access(ctx, lhs);
                            fprintf(ctx->out, " = ");
                            if (rhs_owned) {
                                if (rhs_iface) {
                                    fprintf(ctx->out, "(void*)(");
                                    codegen_expr(ctx, rhs);
                                    fprintf(ctx->out, ").data");
                                } else {
                                    codegen_expr(ctx, rhs);
                                }
                            } else {
                                fprintf(ctx->out, "mylang_retain(");
                                codegen_expr(ctx, rhs);
                                if (rhs_iface) fprintf(ctx->out, ".data");
                                fprintf(ctx->out, ")");
                            }
                            fprintf(ctx->out, ")");
                        }
                    } else if (lt.type_kind == TYPE_INTERFACE && !lt.is_weak) {
                        fprintf(ctx->out, "((void)mylang_release(");
                        codegen_array_access(ctx, lhs);
                        fprintf(ctx->out, ".data), ");
                        codegen_array_access(ctx, lhs);
                        fprintf(ctx->out, " = ");
                        if (rt.type_kind == TYPE_CLASS) {
                            fprintf(ctx->out, "(%s){ ", lt.class_name);
                            if (!rhs_owned) {
                                fprintf(ctx->out, "mylang_retain(");
                            }
                            codegen_expr(ctx, rhs);
                            if (!rhs_owned) {
                                fprintf(ctx->out, ")");
                            }
                            fprintf(ctx->out, ", &%s_%s_vtable }", rt.class_name, lt.class_name);
                        } else if (type_is_null(&rt)) {
                            fprintf(ctx->out, "(%s){ NULL, NULL }", lt.class_name);
                        } else if (rhs_owned) {
                            codegen_expr(ctx, rhs);
                        } else {
                            fprintf(ctx->out, "mylang_retain(");
                            codegen_expr(ctx, rhs);
                            fprintf(ctx->out, ".data), ");
                            codegen_expr(ctx, rhs);
                        }
                        fprintf(ctx->out, ")");
                    } else if (lt.is_weak && lt.type_kind == TYPE_INTERFACE) {
                        fprintf(ctx->out, "((void)mylang_weak_release(");
                        codegen_array_access(ctx, lhs);
                        fprintf(ctx->out, ".wr), ");
                        codegen_array_access(ctx, lhs);
                        fprintf(ctx->out, " = ");
                        if (rt.is_weak && rt.type_kind == TYPE_INTERFACE) {
                            /* weak-to-weak: copy the struct value and take
                               our own share of the WeakRef control block. */
                            char winame[128];
                            c_weak_interface_name(&lt, winame, sizeof(winame));
                            fprintf(ctx->out, "(%s){ mylang_weak_copy(", winame);
                            codegen_expr(ctx, rhs);
                            fprintf(ctx->out, ".wr), ");
                            codegen_expr(ctx, rhs);
                            fprintf(ctx->out, ".vt }");
                        } else if (rt.type_kind == TYPE_INTERFACE) {
                            /* strong interface -> weak interface */
                            if (rhs_owned) {
                                fprintf(ctx->out, "mylang_weakify_%s_owned(", lt.class_name);
                            } else {
                                fprintf(ctx->out, "mylang_weakify_%s(", lt.class_name);
                            }
                            codegen_expr(ctx, rhs);
                            fprintf(ctx->out, ")");
                        } else if (rt.type_kind == TYPE_CLASS) {
                            /* class -> weak interface */
                            if (rhs_owned) {
                                fprintf(ctx->out, "mylang_weakify_%s_from_ptr_owned(", lt.class_name);
                            } else {
                                fprintf(ctx->out, "mylang_weakify_%s_from_ptr(", lt.class_name);
                            }
                            codegen_expr(ctx, rhs);
                            fprintf(ctx->out, ", &%s_%s_vtable)", rt.class_name, lt.class_name);
                        } else if (type_is_null(&rt)) {
                            char winame[128];
                            c_weak_interface_name(&lt, winame, sizeof(winame));
                            fprintf(ctx->out, "(%s){ NULL, NULL }", winame);
                        }
                        fprintf(ctx->out, ")");
                    } else if (lt.is_weak) {
                        /* weak class element: release the old WeakRef, then
                           weak-copy (weak-to-weak) or weakify (strong RHS). */
                        fprintf(ctx->out, "((void)mylang_weak_release(");
                        codegen_array_access(ctx, lhs);
                        fprintf(ctx->out, "), ");
                        if (rt.is_weak) {
                            fprintf(ctx->out, "(");
                            codegen_array_access(ctx, lhs);
                            fprintf(ctx->out, " = mylang_weak_copy(");
                            codegen_expr(ctx, rhs);
                            fprintf(ctx->out, "))");
                        } else if (rhs_owned) {
                            fprintf(ctx->out, "(");
                            codegen_array_access(ctx, lhs);
                            fprintf(ctx->out, " = mylang_weak_init_owned(");
                            codegen_expr(ctx, rhs);
                            fprintf(ctx->out, "))");
                        } else {
                            fprintf(ctx->out, "(");
                            codegen_array_access(ctx, lhs);
                            fprintf(ctx->out, " = mylang_weak_init(");
                            codegen_expr(ctx, rhs);
                            fprintf(ctx->out, "))");
                        }
                        fprintf(ctx->out, ")");
                    } else {
                        /* primitive/struct/bool element */
                        if (type_is_null(&rt)) {
                            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign 'null' to '%s'", type_name(&lt));
                            fprintf(ctx->out, "0 /* invalid null assignment */");
                        } else if (bool_mismatch(&lt, &rt) || enum_mismatch(&lt, &rt)) {
                            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
                            fprintf(ctx->out, "0 /* invalid bool assignment */");
                        } else if (type_is_reference(&rt)) {
                            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
                            fprintf(ctx->out, "0 /* invalid reference assignment */");
                        } else {
                            codegen_array_access(ctx, lhs);
                            fprintf(ctx->out, " = ");
                            codegen_expr(ctx, rhs);
                        }
                    }
                    break;
                }
            }

            if ((lt.is_weak || lt.is_unowned) && lt.type_kind == TYPE_CLASS) {
                /* Weak/unowned class variable or field: the old share is
                   released; RHS is share-copied (weak/unowned) or weakified
                   (strong RHS).  Raw emission: no liveness check on reads of
                   the reference itself. */
                resolve_type(rhs);
                Type rt = rhs->ast_resolved_type;
                int rhs_owned = (expr_is_owned(rhs));

                if (type_is_null(&rt) && lt.is_unowned) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign null to unowned reference");
                    fprintf(ctx->out, "0 /* invalid null assignment */");
                    break;
                }
                if (rt.type_kind != TYPE_CLASS && !type_is_null(&rt) && !rt.is_weak && !rt.is_unowned) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
                    fprintf(ctx->out, "0 /* invalid weak assignment */");
                    break;
                }

                fprintf(ctx->out, "((void)mylang_weak_release(");
                codegen_expr_raw(ctx, lhs);
                fprintf(ctx->out, "), ");
                if (rt.is_weak || rt.is_unowned) {
                    fprintf(ctx->out, "(");
                    codegen_expr_raw(ctx, lhs);
                    fprintf(ctx->out, " = mylang_weak_copy(");
                    codegen_expr_raw(ctx, rhs);
                    fprintf(ctx->out, "))");
                } else if (rhs_owned) {
                    int tmp_id = ctx->assign_tmp_id++;
                    fprintf(ctx->out, "(void* _wassign%d = ", tmp_id);
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ", ");
                    codegen_expr_raw(ctx, lhs);
                    fprintf(ctx->out, " = mylang_weak_init(_wassign%d), mylang_release(_wassign%d))",
                            tmp_id, tmp_id);
                } else {
                    fprintf(ctx->out, "(");
                    codegen_expr_raw(ctx, lhs);
                    fprintf(ctx->out, " = mylang_weak_init(");
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, "))");
                }
                fprintf(ctx->out, ")");
            } else if (lt.type_kind == TYPE_CLASS) {
                resolve_type(rhs);
                Type rt = rhs->ast_resolved_type;
                if (rt.type_kind == TYPE_OBJECT) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign 'object' to '%s'; cast with 'as' first", type_name(&lt));
                    fprintf(ctx->out, "0 /* invalid object assignment */");
                    break;
                }
                if (rt.type_kind != TYPE_CLASS && !type_is_null(&rt) && !expr_is_unowned_lock(rhs)) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
                    fprintf(ctx->out, "0 /* invalid class assignment */");
                    break;
                }
                if (rt.is_weak) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
                    fprintf(ctx->out, "0 /* invalid weak assignment */");
                    break;
                }
                int rhs_owned = (expr_is_owned(rhs));
                int rhs_local = (rhs->ast_kind == AST_IDENT && symtab_lookup(rhs->ast_token.text) != NULL);
                /* Fields own their class references, so assigning a local or
                   parameter to a field must retain the source.  Local-to-local
                   assignment keeps the existing borrow optimization. */
                int lhs_is_field = (lhs->ast_kind == AST_MEMBER_ACCESS);

                fprintf(ctx->out, "((");
                if (!rhs_owned && (!rhs_local || lhs_is_field)) {
                    fprintf(ctx->out, "(void)mylang_retain(");
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, "), ");
                }
                fprintf(ctx->out, "(void)mylang_release(");
                codegen_expr(ctx, lhs);
                fprintf(ctx->out, "), (");
                codegen_expr(ctx, lhs);
                fprintf(ctx->out, " = ");
                codegen_expr(ctx, rhs);
                fprintf(ctx->out, ")))");
            } else if (lt.type_kind == TYPE_OBJECT) {
                /* Same retain-then-release shape as class assignment; an
                   interface RHS contributes its .data pointer. */
                resolve_type(rhs);
                Type rt = rhs->ast_resolved_type;
                int rhs_owned = (expr_is_owned(rhs));
                int rhs_iface = (rt.type_kind == TYPE_INTERFACE && !rt.is_weak);

                if (!rhs_iface && rt.type_kind != TYPE_CLASS &&
                    rt.type_kind != TYPE_OBJECT && !type_is_null(&rt)) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to 'object'", type_name(&rt));
                    fprintf(ctx->out, "0 /* invalid object assignment */");
                    break;
                }

                fprintf(ctx->out, "((");
                if (!rhs_owned) {
                    fprintf(ctx->out, "(void)mylang_retain(");
                    codegen_expr(ctx, rhs);
                    if (rhs_iface) fprintf(ctx->out, ".data");
                    fprintf(ctx->out, "), ");
                }
                fprintf(ctx->out, "(void)mylang_release(");
                codegen_expr(ctx, lhs);
                fprintf(ctx->out, "), (");
                codegen_expr(ctx, lhs);
                fprintf(ctx->out, " = (void*)(");
                codegen_expr(ctx, rhs);
                if (rhs_iface) {
                    fprintf(ctx->out, ").data");
                } else {
                    fprintf(ctx->out, ")");
                }
                fprintf(ctx->out, ")))");
            } else if (lt.is_weak && lt.type_kind == TYPE_INTERFACE) {
                resolve_type(rhs);
                Type rt = rhs->ast_resolved_type;
                int rhs_owned = (expr_is_owned(rhs));

                /* release old weak ref first */
                fprintf(ctx->out, "((void)mylang_weak_release(");
                codegen_expr(ctx, lhs);
                fprintf(ctx->out, ".wr), ");

                if (rt.is_weak && rt.type_kind == TYPE_INTERFACE) {
                    /* weak -> weak copy */
                    fprintf(ctx->out, "(");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".wr = mylang_weak_copy(");
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ".wr), ");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".vt = ");
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ".vt))");
                } else if (rt.type_kind == TYPE_INTERFACE) {
                    if (rhs_owned) {
                        int tmp_id = ctx->assign_tmp_id++;
                        fprintf(ctx->out, "(%s _wassign%d = ", lt.class_name, tmp_id);
                        codegen_expr(ctx, rhs);
                        fprintf(ctx->out, ", ");
                        codegen_expr(ctx, lhs);
                        fprintf(ctx->out, ".wr = mylang_weak_init(_wassign%d.data), ", tmp_id);
                        codegen_expr(ctx, lhs);
                        fprintf(ctx->out, ".vt = _wassign%d.vtable, mylang_release(_wassign%d.data))", tmp_id, tmp_id);
                    } else {
                        fprintf(ctx->out, "(");
                        codegen_expr(ctx, lhs);
                        fprintf(ctx->out, ".wr = mylang_weak_init(");
                        codegen_expr(ctx, rhs);
                        fprintf(ctx->out, ".data), ");
                        codegen_expr(ctx, lhs);
                        fprintf(ctx->out, ".vt = ");
                        codegen_expr(ctx, rhs);
                        fprintf(ctx->out, ".vtable))");
                    }
                } else if (rt.type_kind == TYPE_CLASS) {
                    if (rhs_owned) {
                        int tmp_id = ctx->assign_tmp_id++;
                        fprintf(ctx->out, "(void* _wassign%d = ", tmp_id);
                        codegen_expr(ctx, rhs);
                        fprintf(ctx->out, ", ");
                        codegen_expr(ctx, lhs);
                        fprintf(ctx->out, ".wr = mylang_weak_init(_wassign%d), ", tmp_id);
                        codegen_expr(ctx, lhs);
                        fprintf(ctx->out, ".vt = &%s_%s_vtable, mylang_release(_wassign%d))",
                                rt.class_name, lt.class_name, tmp_id);
                    } else {
                        fprintf(ctx->out, "(");
                        codegen_expr(ctx, lhs);
                        fprintf(ctx->out, ".wr = mylang_weak_init(");
                        codegen_expr(ctx, rhs);
                        fprintf(ctx->out, "), ");
                        codegen_expr(ctx, lhs);
                        fprintf(ctx->out, ".vt = &%s_%s_vtable))",
                                rt.class_name, lt.class_name);
                    }
                } else if (type_is_null(&rt)) {
                    fprintf(ctx->out, "(");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".wr = NULL), (");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".vt = NULL))");
                } else {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign to weak interface from this type");
                    fprintf(ctx->out, "0)");
                }
            } else if (lt.type_kind == TYPE_INTERFACE) {
                resolve_type(rhs);
                Type rt = rhs->ast_resolved_type;
                int rhs_owned = (expr_is_owned(rhs));

                fprintf(ctx->out, "((void)");
                if (rt.type_kind == TYPE_CLASS) {
                    if (!rhs_owned) {
                        fprintf(ctx->out, "mylang_retain(");
                        codegen_expr(ctx, rhs);
                        fprintf(ctx->out, "), ");
                    }
                    fprintf(ctx->out, "mylang_release(");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".data), (");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".data = ");
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ", ");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".vtable = &%s_%s_vtable", rt.class_name, lt.class_name);
                    fprintf(ctx->out, "))");
                } else if (type_is_null(&rt)) {
                    fprintf(ctx->out, "mylang_release(");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".data), (");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".data = NULL), (");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".vtable = NULL))");
                } else {
                    if (!rhs_owned) {
                        fprintf(ctx->out, "mylang_retain(");
                        codegen_expr(ctx, rhs);
                        fprintf(ctx->out, ".data), ");
                    }
                    fprintf(ctx->out, "mylang_release(");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ".data), (");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, " = ");
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ")))");
                }
            } else {
                /* primitive/struct/bool plain assignment */
                resolve_type(rhs);
                Type rt = rhs->ast_resolved_type;
                if (type_is_null(&rt)) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign 'null' to '%s'", type_name(&lt));
                    fprintf(ctx->out, "0 /* invalid null assignment */");
                } else if (bool_mismatch(&lt, &rt) || enum_mismatch(&lt, &rt)) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
                    fprintf(ctx->out, "0 /* invalid bool assignment */");
                } else if (type_is_reference(&rt)) {
                    codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
                    fprintf(ctx->out, "0 /* invalid reference assignment */");
                } else if (struct_has_ref_fields(&lt)) {
                    /* Struct with reference fields: retain the new value's
                       shares, release the old value's, then copy.  Retain
                       first so self-assignment is safe.  Owned RHS (a call
                       result) moves without an extra retain. */
                    if (!expr_is_owned(rhs)) {
                        fprintf(ctx->out, "((void)_mylang_retain_%s(&(", lt.class_name);
                        codegen_expr(ctx, rhs);
                        fprintf(ctx->out, ")), ");
                    } else {
                        fprintf(ctx->out, "(");
                    }
                    fprintf(ctx->out, "(void)_mylang_release_%s(&(", lt.class_name);
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, ")), (");
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, " = ");
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, "))");
                } else {
                    codegen_expr(ctx, lhs);
                    fprintf(ctx->out, " = ");
                    codegen_expr(ctx, rhs);
                }
            }
            break;
        }
        case AST_CALL:
            codegen_call(ctx, node);
            break;
        case AST_ARRAY_ACCESS:
            codegen_array_access(ctx, node);
            break;
        case AST_MEMBER_ACCESS:
            codegen_member_access(ctx, node);
            break;
        case AST_NEW:
            codegen_new(ctx, node);
            break;
        case AST_FSTRING:
            /* Lowered to a temporary by emit_fstring_preambles. */
            fprintf(ctx->out, "%s", node->ast_temp_name);
            break;
        case AST_AS_CAST: {
            AstNode* obj = node->ast_children[0];
            resolve_type(obj);
            Type target = node->ast_resolved_type;
            if (type_is_null(&obj->ast_resolved_type)) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot cast null");
                fprintf(ctx->out, "NULL /* invalid null cast */");
            } else if (target.type_kind == TYPE_CLASS &&
                       obj->ast_resolved_type.type_kind == TYPE_OBJECT) {
                /* object -> class: check the concrete type id in the object
                   header; a null object casts to NULL. */
                ClassInfo* ci = symtab_find_class(target.class_name);
                const char* obj_name = (obj->ast_temp_name[0] != '\0') ? obj->ast_temp_name : NULL;
                fprintf(ctx->out, "((");
                if (obj_name) fprintf(ctx->out, "%s", obj_name);
                else codegen_expr(ctx, obj);
                fprintf(ctx->out, ") == NULL ? NULL : (mylang_obj_hdr(");
                if (obj_name) fprintf(ctx->out, "%s", obj_name);
                else codegen_expr(ctx, obj);
                fprintf(ctx->out, ")->type_id == %u ? (%s*)(",
                        ci ? (unsigned)ci->type_id : 0, target.class_name);
                if (obj_name) fprintf(ctx->out, "%s", obj_name);
                else codegen_expr(ctx, obj);
                fprintf(ctx->out, ") : NULL))");
            } else if (target.type_kind == TYPE_CLASS) {
                ClassInfo* ci = symtab_find_class(target.class_name);
                /* Use a temporary if the object expression was hoisted by
                   emit_subexpr_temps; otherwise codegen_expr would re-evaluate
                   a call-like subexpression (e.g. w.lock()) twice. */
                const char* obj_name = (obj->ast_temp_name[0] != '\0') ? obj->ast_temp_name : NULL;
                fprintf(ctx->out, "((");
                if (obj_name) fprintf(ctx->out, "%s", obj_name);
                else codegen_expr(ctx, obj);
                fprintf(ctx->out, ").vtable->concrete_type_id == %u ? (%s*)(",
                        ci ? (unsigned)ci->type_id : 0, target.class_name);
                if (obj_name) fprintf(ctx->out, "%s", obj_name);
                else codegen_expr(ctx, obj);
                fprintf(ctx->out, ").data : NULL)");
            } else if (target.type_kind == TYPE_ENUM &&
                       type_is_integer(&obj->ast_resolved_type)) {
                /* integer -> enum: plain C cast, no runtime check. */
                fprintf(ctx->out, "((%s)(", c_base_name(&target));
                codegen_expr(ctx, obj);
                fprintf(ctx->out, "))");
            } else if (type_is_integer(&target) &&
                       obj->ast_resolved_type.type_kind == TYPE_ENUM) {
                /* enum -> integer: plain C cast. */
                fprintf(ctx->out, "((%s)(", c_base_name(&target));
                codegen_expr(ctx, obj);
                fprintf(ctx->out, "))");
            } else if (target.type_kind == TYPE_ENUM ||
                       obj->ast_resolved_type.type_kind == TYPE_ENUM) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                                     "cannot cast '%s' to '%s'",
                                     type_name(&obj->ast_resolved_type), type_name(&target));
                fprintf(ctx->out, "0 /* invalid enum cast */");
            } else {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "'as' target must be a class type");
                fprintf(ctx->out, "/* as-cast unsupported target */");
            }
            break;
        }
        default:
            fprintf(ctx->out, "/* ??? */");
            break;
    }
}

/* -- bounds checking ------------------------------------------------- */

static void indent_line(CodegenContext* ctx, int indent);

static void emit_bounds_checks(CodegenContext* ctx, AstNode* expr, int indent) {
    if (!expr) return;

    /* Dynamic arrays perform bounds checks inside mylang_array_at.  Fixed-size
       arrays have been removed from the language. */
    (void)ctx;
    (void)ctx->out;
    (void)indent;

    int i;
    for (i = 0; i < expr->ast_child_count; i++) {
        emit_bounds_checks(ctx, expr->ast_children[i], indent);
    }
    emit_bounds_checks(ctx, expr->next, indent);
}

/* -- caller-side arg guard helpers ----------------------------------- */


static int call_needs_guard(AstNode* arg) {
    if (!arg) return 0;
    if (arg->ast_kind == AST_REF_ARG) return 0;
    resolve_type(arg);
    TypeKind k = arg->ast_resolved_type.type_kind;
    /* Owned struct call results with owning fields need a guard temporary so
       their shares can be released once at scope exit. */
    if (k == TYPE_STRUCT && struct_has_ref_fields(&arg->ast_resolved_type) &&
        arg->ast_kind == AST_CALL) return 1;
    if (k != TYPE_CLASS && k != TYPE_INTERFACE && k != TYPE_OBJECT) return 0;
    /* unowned reads are borrowed and side-effect-free; they are checked
       inline at the use site and never need a guard temporary. */
    if (arg->ast_resolved_type.is_unowned) return 0;
    if (arg->ast_resolved_type.is_array) return 0;
    if (arg->ast_kind == AST_ASSIGN) return 0;
    if (arg->ast_kind == AST_IDENT && symtab_lookup(arg->ast_token.text)) return 0;
    return 1;
}

static int guard_needs_retain(AstNode* node) {
    /* Calls/new already return an owned (+1) reference; f-string accumulators
       are owned by their cleanup-tracked temp. */
    return !guard_expr_is_owned(node);
}

static int return_expr_needs_retain(AstNode* node) {
    /* Calls/new already produce an owned (+1) reference for the caller.
       Local variables, fields, and array elements must be retained because
       the caller will release the returned value and the original owner still
       holds its own reference. */
    return !expr_is_owned(node);
}

/* Sub-expressions that must be hoisted into a temporary so they are evaluated
   exactly once.  Only call sites can add reference counts; lvalues, weak refs,
   arrays and 'new' are left to their surrounding statement. */
static int subexpr_needs_temp(AstNode* node) {
    if (!node) return 0;
    resolve_type(node);
    Type* t = &node->ast_resolved_type;
    if (t->is_weak || t->is_unowned) return 0;
    if (t->is_array) return 0;
    /* Owned struct call results with owning fields must also be hoisted so
       their shares can be released once at scope exit. */
    if (t->type_kind == TYPE_STRUCT && struct_has_ref_fields(t)) {
        return node->ast_kind == AST_CALL;
    }
    if (t->type_kind != TYPE_CLASS && t->type_kind != TYPE_INTERFACE &&
        t->type_kind != TYPE_OBJECT) return 0;
    return node->ast_kind == AST_CALL;
}

static void cleanup_add(CodegenContext* ctx, const char* name, int is_weak, int is_interface);
static void cleanup_add_struct_dtor(CodegenContext* ctx, const char* name, const char* struct_name);

/* Returns 1 when the argument at arg_index of the call is pushed (owned) into
   an array of structs with reference fields.  Push moves an owned value into
   the slot (no retain), so the guard temporary must not release it. */
static int call_arg_consumed_by_struct_push(AstNode* call, int arg_index) {
    if (call->ast_children[0]->ast_kind != AST_MEMBER_ACCESS) return 0;
    AstNode* mem = call->ast_children[0];
    if (strcmp(mem->ast_token.text, "push") != 0) return 0;
    AstNode* obj = mem->ast_children[0];
    resolve_type(obj);
    if (!type_is_ref_struct_array(&obj->ast_resolved_type)) return 0;
    AstNode* a = call->ast_children[1];
    int i = 0;
    while (a && i < arg_index) { a = a->next; i++; }
    if (!a) return 0;
    return expr_is_owned(a);
}

/* Returns 1 when the argument at arg_index of the call is passed to a weak
   interface parameter.  An owned argument there is consumed by the
   mylang_weakify_*_owned conversion (which releases it), so it must not be
   released again via the cleanup list. */
static int call_arg_consumed_by_weak_iface(AstNode* call, int arg_index) {
    AstNode* callee = call->ast_children[0];
    const Type* pt = NULL;
    if (callee->ast_kind == AST_MEMBER_ACCESS) {
        AstNode* obj = callee->ast_children[0];
        const char* mname = callee->ast_token.text;
        MethodInfo* smi = static_call_method(callee, NULL);
        if (smi && smi->method_is_static) {
            if (arg_index < smi->param_count) pt = &smi->param_types[arg_index];
        } else {
        resolve_type(obj);
        if (obj->ast_resolved_type.type_kind == TYPE_CLASS) {
            ClassInfo* ci = obj->ast_resolved_type.type_arg_count > 0
                ? symtab_instantiate_class_from_type(&obj->ast_resolved_type)
                : symtab_find_class(obj->ast_resolved_type.class_name);
            MethodInfo* mi = ci ? symtab_find_method_in_class(ci, mname) : NULL;
            if (mi && arg_index < mi->param_count) pt = &mi->param_types[arg_index];
        } else if (obj->ast_resolved_type.type_kind == TYPE_INTERFACE) {
            InterfaceInfo* ii = symtab_find_interface(obj->ast_resolved_type.class_name);
            InterfaceMethodInfo* im = ii ? symtab_find_interface_method(ii, mname) : NULL;
            if (im && arg_index < im->param_count) pt = &im->param_types[arg_index];
        }
        }
    } else if (callee->ast_kind == AST_IDENT) {
        FuncInfo* fi = symtab_find_func(callee->ast_token.text);
        if (fi && arg_index < fi->param_count) pt = &fi->param_types[arg_index];
    }
    return pt && pt->is_weak && pt->type_kind == TYPE_INTERFACE;
}

/* Evaluate each guarded class subexpression into a temporary once, so we do
   not re-evaluate side-effecting expressions (e.g. method calls) when the
   caller-side retain/release guards are emitted.

   at_value_root marks positions whose value ownership is consumed by the
   surrounding statement: the initializer of a variable declaration, the
   expression of a return statement, the root of an expression statement, and
   the RHS of an assignment.  Owned temporaries at those positions must NOT be
   released locally.  Owned temporaries everywhere else (call arguments,
   method receivers, f-string interpolation parts) are only borrowed by the
   call, so they are tracked on the cleanup list and released once at scope
   exit; this covers if/while/for conditions and return paths where no
   post-call guard release is emitted.  owned_consumed marks arguments whose
   ownership is consumed by the call itself (weak interface parameters). */
static void emit_guarded_temp_decls_impl(CodegenContext* ctx, AstNode* expr, int indent,
                                         int at_value_root, int owned_consumed) {
    if (!expr) return;

    /* F-strings are lowered by emit_fstring_preambles.  Still walk the next
       chain so sibling arguments after an f-string are processed. */
    if (expr->ast_kind == AST_FSTRING) {
        emit_guarded_temp_decls_impl(ctx, expr->next, indent, at_value_root, 0);
        return;
    }

    /* Do not extract the LHS of an assignment into a temporary; it must remain
       an lvalue so the assignment writes to the real location.  The RHS value
       is consumed by the assignment target. */
    if (expr->ast_kind == AST_ASSIGN) {
        emit_guarded_temp_decls_impl(ctx, expr->ast_children[1], indent, 1, 0);
        emit_guarded_temp_decls_impl(ctx, expr->next, indent, at_value_root, 0);
        return;
    }

    int i;
    for (i = 0; i < expr->ast_child_count; i++) {
        if (expr->ast_kind == AST_CALL && i == 1) {
            /* Call arguments: per-argument ownership consumption depends on
               the target parameter type. */
            AstNode* a = expr->ast_children[1];
            int idx = 0;
            while (a) {
                AstNode* nxt = a->next;
                a->next = NULL;
                emit_guarded_temp_decls_impl(ctx, a, indent, 0,
                                             call_arg_consumed_by_weak_iface(expr, idx) ||
                                             call_arg_consumed_by_struct_push(expr, idx));
                a->next = nxt;
                a = nxt;
                idx++;
            }
        } else {
            emit_guarded_temp_decls_impl(ctx, expr->ast_children[i], indent, 0, 0);
        }
    }
    emit_guarded_temp_decls_impl(ctx, expr->next, indent, at_value_root, 0);

    if (call_needs_guard(expr) && expr->ast_temp_name[0] == '\0') {
        int id = ctx->guard_tmp_id++;
        int n = snprintf(expr->ast_temp_name, sizeof(expr->ast_temp_name), "_g%d", id);
        CHECK_SNPRINTF(n, sizeof(expr->ast_temp_name), "guard temporary name too long");

        char tbuf[128];
        c_type_str(&expr->ast_resolved_type, tbuf, sizeof(tbuf));
        indent_line(ctx, indent);
        fprintf(ctx->out, "%s %s = ", tbuf, expr->ast_temp_name);

        /* Evaluate the original expression without using its own temp name. */
        char saved[NAME_BUF_SIZE];
        CHECK_STRSCPY(strscpy(saved, expr->ast_temp_name, sizeof(saved)),
                      "guard temporary name too long");
        expr->ast_temp_name[0] = '\0';
        codegen_expr(ctx, expr);
        CHECK_STRSCPY(strscpy(expr->ast_temp_name, saved, sizeof(expr->ast_temp_name)),
                      "guard temporary name too long");

        fprintf(ctx->out, ";\n");

        if (!at_value_root && !owned_consumed && expr_is_owned(expr)) {
            if (expr->ast_resolved_type.type_kind == TYPE_STRUCT) {
                cleanup_add_struct_dtor(ctx, expr->ast_temp_name,
                                        expr->ast_resolved_type.class_name);
            } else {
                cleanup_add(ctx, expr->ast_temp_name, 0,
                            expr->ast_resolved_type.type_kind == TYPE_INTERFACE);
            }
        }
    }
}

static void emit_guarded_temp_decls(CodegenContext* ctx, AstNode* expr, int indent) {
    emit_guarded_temp_decls_impl(ctx, expr, indent, 1, 0);
}

/* Extract an owned class/interface call into a temporary variable so it is
   evaluated exactly once.  The temporary is released via cleanup. */
static void extract_owned_call_temp(CodegenContext* ctx, AstNode* node, int indent) {
    if (!node || node->ast_temp_name[0] != '\0') return;
    if (!subexpr_needs_temp(node)) return;

    int id = ctx->subexpr_tmp_id++;
    int n = snprintf(node->ast_temp_name, sizeof(node->ast_temp_name), "_i%d", id);
    CHECK_SNPRINTF(n, sizeof(node->ast_temp_name), "subexpr temporary name too long");

    char tbuf[128];
    c_type_str(&node->ast_resolved_type, tbuf, sizeof(tbuf));
    indent_line(ctx, indent);
    fprintf(ctx->out, "%s %s = ", tbuf, node->ast_temp_name);

    /* Evaluate the original expression without using its own temp name. */
    char saved[NAME_BUF_SIZE];
    CHECK_STRSCPY(strscpy(saved, node->ast_temp_name, sizeof(saved)),
                  "subexpr temporary name too long");
    node->ast_temp_name[0] = '\0';
    codegen_expr(ctx, node);
    CHECK_STRSCPY(strscpy(node->ast_temp_name, saved, sizeof(node->ast_temp_name)),
                  "subexpr temporary name too long");

    fprintf(ctx->out, ";\n");

    if (node->ast_resolved_type.type_kind == TYPE_INTERFACE) {
        cleanup_add(ctx, node->ast_temp_name, 0, 1);
    } else if (node->ast_resolved_type.type_kind == TYPE_STRUCT) {
        cleanup_add_struct_dtor(ctx, node->ast_temp_name, node->ast_resolved_type.class_name);
    } else {
        cleanup_add(ctx, node->ast_temp_name, 0, 0);
    }
}

/* Extract owned class/interface subexpression calls into temporaries so they
   are not re-evaluated (which leaks reference counts).  Direct call arguments
   are left for emit_guarded_temp_decls to handle. */
static void emit_subexpr_temps_impl(CodegenContext* ctx, AstNode* node, int indent, int extract_root) {
    if (!node) return;

    /* F-strings are lowered by emit_fstring_preambles; do not extract parts
       from inside them here.  Still walk the next chain so sibling arguments
       after an f-string are processed. */
    if (node->ast_kind == AST_FSTRING) {
        emit_subexpr_temps_impl(ctx, node->next, indent, extract_root);
        return;
    }

    if (node->ast_kind == AST_AS_CAST) {
        emit_subexpr_temps_impl(ctx, node->ast_children[0], indent, 1);
        AstNode* obj = node->ast_children[0];
        if (subexpr_needs_temp(obj)) {
            extract_owned_call_temp(ctx, obj, indent);
        }
        emit_subexpr_temps_impl(ctx, node->next, indent, 1);
        return;
    }

    if (node->ast_kind == AST_BINARY) {
        int i;
        for (i = 0; i < node->ast_child_count; i++) {
            emit_subexpr_temps_impl(ctx, node->ast_children[i], indent, 1);
        }
        for (i = 0; i < node->ast_child_count; i++) {
            AstNode* child = node->ast_children[i];
            if (subexpr_needs_temp(child)) {
                extract_owned_call_temp(ctx, child, indent);
            }
        }
        emit_subexpr_temps_impl(ctx, node->next, indent, 1);
        return;
    }

    if (node->ast_kind == AST_MEMBER_ACCESS) {
        emit_subexpr_temps_impl(ctx, node->ast_children[0], indent, 1);
        AstNode* obj = node->ast_children[0];
        if (subexpr_needs_temp(obj)) {
            extract_owned_call_temp(ctx, obj, indent);
        }
        emit_subexpr_temps_impl(ctx, node->next, indent, 1);
        return;
    }

    if (node->ast_kind == AST_CALL) {
        /* Recurse into callee for method-call receiver subexpressions. */
        if (node->ast_children[0]->ast_kind == AST_MEMBER_ACCESS) {
            emit_subexpr_temps_impl(ctx, node->ast_children[0], indent, 1);
        }
        /* Top-level arguments are handled by emit_guarded_temp_decls for
           lifetime management.  However, nested subexpressions inside
           arguments (e.g. the object of an 'as' cast or an interface method
           receiver) may be evaluated multiple times by their parent, so we
           extract those into temporaries.  We pass extract_root=0 so the
           argument root itself is not extracted here. */
        AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
        while (args) {
            emit_subexpr_temps_impl(ctx, args, indent, 0);
            args = args->next;
        }
        emit_subexpr_temps_impl(ctx, node->next, indent, 1);
        return;
    }

    if (node->ast_kind == AST_ASSIGN) {
        /* LHS must remain an lvalue.  RHS is evaluated into its destination,
           so only nested subexpressions inside RHS need extraction. */
        emit_subexpr_temps_impl(ctx, node->ast_children[1], indent, 1);
        emit_subexpr_temps_impl(ctx, node->next, indent, 1);
        return;
    }

    if (node->ast_kind == AST_NEW) {
        /* 'new' is handled by its surrounding statement; only nested size
           expressions need subexpression extraction. */
        emit_subexpr_temps_impl(ctx, node->ast_children[0], indent, 1);
        emit_subexpr_temps_impl(ctx, node->next, indent, 1);
        return;
    }

    int i;
    for (i = 0; i < node->ast_child_count; i++) {
        emit_subexpr_temps_impl(ctx, node->ast_children[i], indent, 1);
    }
    emit_subexpr_temps_impl(ctx, node->next, indent, 1);

    if (extract_root && subexpr_needs_temp(node)) {
        extract_owned_call_temp(ctx, node, indent);
    }
}

static void emit_subexpr_temps(CodegenContext* ctx, AstNode* node, int indent) {
    emit_subexpr_temps_impl(ctx, node, indent, 1);
}

static int type_is_string(const Type* t) {
    return t->type_kind == TYPE_CLASS && strcmp(t->class_name, "String") == 0;
}

static const char* fstring_append_method(const Type* t) {
    if (type_is_string(t)) return "append_string";
    switch (t->type_kind) {
        case TYPE_I8:  return "append_char";
        case TYPE_I16:
        case TYPE_I32: return "append_i32";
        case TYPE_I64: return "append_i64";
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32: return "append_u32";
        case TYPE_U64: return "append_u64";
        case TYPE_F32: return "append_f32";
        case TYPE_F64: return "append_f64";
        case TYPE_BOOL: return "append_bool";
        default:       return NULL;
    }
}

static const char* fstring_temp_name(const char* prefix, int id) {
    char* buf = (char*)calloc(1, 64);
    int n = snprintf(buf, 64, "%s%d", prefix, id);
    CHECK_SNPRINTF(n, 64, "f-string temp name too long");
    return buf;
}

/* Look up a toString method suitable for f-string interpolation on a class
   type.  Returns the class (instantiating generics if needed) through ci_out
   when a matching 'string toString()' method exists. */
static ClassInfo* fstring_find_tostring_class(Type* t) {
    ClassInfo* ci = NULL;
    if (t->type_arg_count > 0) {
        ci = symtab_instantiate_class_from_type(t);
    } else {
        ci = symtab_find_class(t->class_name);
    }
    if (!ci) return NULL;
    MethodInfo* mi = symtab_find_method_in_class(ci, "toString");
    if (!mi || mi->param_count != 0 || !type_is_string(&mi->return_type)) {
        return NULL;
    }
    return ci;
}

/* Emit the String accumulator setup for an f-string before the statement that
   uses it.  Each interpolated expression is evaluated once and in source
   order.  The accumulator String itself is the result; the AST_FSTRING node
   keeps its temp name. */
static void codegen_fstring_preamble(CodegenContext* ctx, AstNode* node, int indent) {
    if (!node || node->ast_kind != AST_FSTRING) return;
    if (node->ast_temp_name[0] != '\0') return;

    /* Nested f-strings inside interpolated expressions are lowered first. */
    AstNode* part = node->ast_children[0];
    while (part) {
        if (part->ast_kind == AST_FSTRING) {
            codegen_fstring_preamble(ctx, part, indent);
        }
        part = part->next;
    }

    const char* acc = fstring_temp_name("_fs", ctx->fstring_tmp_id++);
    indent_line(ctx, indent);
    if (ctx->xor_strings) {
        fprintf(ctx->out, "String* %s = mylang_string_new_encrypted(MYLANG_TID_String, NULL, 0, 1);\n", acc);
    } else {
        fprintf(ctx->out, "String* %s = mylang_string_new(MYLANG_TID_String, \"\");\n", acc);
    }
    cleanup_add(ctx, acc, 0, 0);

    part = node->ast_children[0];
    while (part) {
        if (part->ast_kind == AST_STRING_LIT) {
            /* Literal segments are appended directly from the C string; no
               temporary String object is allocated. */
            if (ctx->xor_strings) {
                const char* s = part->ast_token.text;
                size_t len = strlen(s);
                if (len == 0) {
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "mylang_string_append_cstr_encrypted(%s, NULL, 0, 1);\n", acc);
                } else {
                    int id = part->xor_str_id;
                    if (id == 0) {
                        id = ++ctx->xor_str_id;
                        part->xor_str_id = id;
                        emit_xor_string_array_decl(ctx, s, id);
                    }
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "mylang_string_append_cstr_encrypted(%s, _xs%d, %zu, %u);\n",
                            acc, id, len, (unsigned)xor_string_key(id));
                }
            } else {
                indent_line(ctx, indent);
                fprintf(ctx->out, "mylang_string_append_cstr(%s, \"", acc);
                emit_c_string_literal(ctx, part->ast_token.text);
                fprintf(ctx->out, "\");\n");
            }
        } else {
            /* Owned class/interface subexpressions inside the interpolation
               are extracted into temporaries first so they are evaluated
               once.  Detach the part from the f-string part list so the
               helpers do not walk into following sibling parts. */
            AstNode* saved_next = part->next;
            part->next = NULL;
            emit_subexpr_temps(ctx, part, indent);
            /* The part value is only borrowed by the append call, so owned
               temporaries must be released via the cleanup list. */
            emit_guarded_temp_decls_impl(ctx, part, indent, 0, 0);
            part->next = saved_next;

            resolve_type(part);
            Type t = part->ast_resolved_type;
            const char* method = fstring_append_method(&t);
            if (method) {
                if (type_is_string(&t) && part->ast_temp_name[0] == '\0' &&
                    expr_is_owned(part)) {
                    /* Owned string not yet in a tracked temp (e.g. a string
                       literal used directly as an interpolation): keep it in
                       a cleanup-tracked temp so it is released once. */
                    const char* expr_name = fstring_temp_name("_fse", ctx->fstring_tmp_id++);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "String* %s = ", expr_name);
                    codegen_expr(ctx, part);
                    fprintf(ctx->out, ";\n");
                    cleanup_add(ctx, expr_name, 0, 0);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "String_append_string(%s, %s);\n", acc, expr_name);
                } else {
                    /* Borrowed strings, tracked temps, and primitives are
                       appended directly without releasing the source. */
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "String_%s(%s, ", method, acc);
                    codegen_expr(ctx, part);
                    fprintf(ctx->out, ");\n");
                }
            } else if (t.type_kind == TYPE_CLASS) {
                /* IToString: call the class's toString method. */
                ClassInfo* ci = fstring_find_tostring_class(&t);
                if (!ci) {
                    codegen_report_error(ctx, part->ast_token.line, part->ast_token.col, "cannot interpolate type '%s'; implement IToString ('string toString()')", t.class_name);
                } else if (!member_visible(ctx, ci->name,
                                           symtab_find_method_in_class(ci, "toString")->is_private)) {
                    codegen_report_error(ctx, part->ast_token.line, part->ast_token.col, "cannot interpolate type '%s'; toString() is private", t.class_name);
                } else {
                    const char* obj_name = NULL;
                    if (part->ast_temp_name[0] != '\0') {
                        /* Already hoisted into a cleanup-tracked temp. */
                        obj_name = part->ast_temp_name;
                    } else if (expr_is_owned(part)) {
                        obj_name = fstring_temp_name("_fso", ctx->fstring_tmp_id++);
                        char tbuf[128];
                        c_type_str(&t, tbuf, sizeof(tbuf));
                        indent_line(ctx, indent);
                        fprintf(ctx->out, "%s %s = ", tbuf, obj_name);
                        codegen_expr(ctx, part);
                        fprintf(ctx->out, ";\n");
                        cleanup_add(ctx, obj_name, 0, 0);
                    }
                    const char* str_name = fstring_temp_name("_fss", ctx->fstring_tmp_id++);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "String* %s = %s_toString(", str_name, class_c_name(ci));
                    if (obj_name) {
                        fprintf(ctx->out, "%s", obj_name);
                    } else {
                        codegen_expr(ctx, part);
                    }
                    fprintf(ctx->out, ");\n");
                    cleanup_add(ctx, str_name, 0, 0);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "String_append_string(%s, %s);\n", acc, str_name);
                }
            } else if (t.type_kind == TYPE_INTERFACE) {
                /* IToString through dynamic dispatch. */
                InterfaceInfo* ii = symtab_find_interface(t.class_name);
                InterfaceMethodInfo* im = ii ? symtab_find_interface_method(ii, "toString") : NULL;
                if (!im || im->param_count != 0 || !type_is_string(&im->return_type)) {
                    codegen_report_error(ctx, part->ast_token.line, part->ast_token.col, "cannot interpolate interface '%s'; it has no 'string toString()'", t.class_name);
                } else {
                    const char* obj_name = NULL;
                    if (part->ast_temp_name[0] != '\0') {
                        obj_name = part->ast_temp_name;
                    } else if (expr_is_owned(part)) {
                        obj_name = fstring_temp_name("_fso", ctx->fstring_tmp_id++);
                        char tbuf[128];
                        c_type_str(&t, tbuf, sizeof(tbuf));
                        indent_line(ctx, indent);
                        fprintf(ctx->out, "%s %s = ", tbuf, obj_name);
                        codegen_expr(ctx, part);
                        fprintf(ctx->out, ";\n");
                        cleanup_add(ctx, obj_name, 0, 1);
                    }
                    const char* str_name = fstring_temp_name("_fss", ctx->fstring_tmp_id++);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "String* %s = (", str_name);
                    if (obj_name) {
                        fprintf(ctx->out, "%s).vtable->toString((%s).data", obj_name, obj_name);
                    } else {
                        codegen_expr(ctx, part);
                        fprintf(ctx->out, ").vtable->toString((");
                        codegen_expr(ctx, part);
                        fprintf(ctx->out, ").data");
                    }
                    fprintf(ctx->out, ");\n");
                    cleanup_add(ctx, str_name, 0, 0);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "String_append_string(%s, %s);\n", acc, str_name);
                }
            } else {
                codegen_report_error(ctx, part->ast_token.line, part->ast_token.col, "cannot interpolate value of type '%s' in f-string", type_name(&t));
            }
        }
        part = part->next;
    }

    /* The accumulator String is the result of the f-string expression. */
    CHECK_STRSCPY(strscpy(node->ast_temp_name, acc, sizeof(node->ast_temp_name)),
                  "f-string result temp name too long");
}

/* Lower every f-string contained in a statement before the statement is
   emitted.  codegen_expr for AST_FSTRING then just outputs the result temp. */
static void emit_fstring_preambles(CodegenContext* ctx, AstNode* node, int indent) {
    if (!node) return;
    if (node->ast_kind == AST_FSTRING) {
        codegen_fstring_preamble(ctx, node, indent);
        return;
    }
    int i;
    for (i = 0; i < node->ast_child_count; i++) {
        emit_fstring_preambles(ctx, node->ast_children[i], indent);
    }
    emit_fstring_preambles(ctx, node->next, indent);
}

static void emit_call_guards(CodegenContext* ctx, AstNode* expr, int is_retain) {
    if (!expr) return;
    if (expr->ast_kind == AST_CALL) {
        AstNode* callee = expr->ast_children[0];
        AstNode* args  = (expr->ast_child_count > 1) ? expr->ast_children[1] : NULL;

        /* Owned arguments/receivers are tracked on the cleanup list by
           emit_guarded_temp_decls and released at scope exit; only borrowed
           (non-owned) ones need a retain/release pair around the call. */
        if (callee->ast_kind == AST_MEMBER_ACCESS) {
            AstNode* obj = callee->ast_children[0];
            if (call_needs_guard(obj) && guard_needs_retain(obj)) {
                /* Interface temps are fat-pointer structs: retain .data. */
                int is_iface = obj->ast_resolved_type.type_kind == TYPE_INTERFACE;
                if (is_retain) {
                    fprintf(ctx->out, is_iface ? "mylang_retain(%s.data); " : "mylang_retain(%s); ", obj->ast_temp_name);
                } else {
                    fprintf(ctx->out, is_iface ? "mylang_release(%s.data); " : "mylang_release(%s); ", obj->ast_temp_name);
                }
            }
        }
        AstNode* a = args;
        while (a) {
            if (call_needs_guard(a) && guard_needs_retain(a)) {
                int is_iface = a->ast_resolved_type.type_kind == TYPE_INTERFACE;
                if (is_retain) {
                    fprintf(ctx->out, is_iface ? "mylang_retain(%s.data); " : "mylang_retain(%s); ", a->ast_temp_name);
                } else {
                    fprintf(ctx->out, is_iface ? "mylang_release(%s.data); " : "mylang_release(%s); ", a->ast_temp_name);
                }
            }
            a = a->next;
        }
    }
    int i;
    for (i = 0; i < expr->ast_child_count; i++) emit_call_guards(ctx, expr->ast_children[i], is_retain);
}

static void emit_stmt_call_retains(CodegenContext* ctx, AstNode* expr, int indent) {
    indent_line(ctx, indent);
    emit_call_guards(ctx, expr, 1);
    fprintf(ctx->out, "\n");
}
static void emit_stmt_call_releases(CodegenContext* ctx, AstNode* expr, int indent) {
    indent_line(ctx, indent);
    emit_call_guards(ctx, expr, 0);
    fprintf(ctx->out, "\n");
}

/* --- CODE GENERATION -- statements --- */

/* -- refcount cleanup list ------------------------------------------- */



static void cleanup_add(CodegenContext* ctx, const char* name, int is_weak, int is_interface) {
    if (ctx->cleanup_count < MAX_CLEANUP) {
        CleanupEntry* e = &ctx->cleanup_entries[ctx->cleanup_count];
        e->name = name;
        e->is_weak = is_weak;
        e->is_interface = is_interface;
        e->is_interface_array = 0;
        e->interface_array_size = 0;
        e->is_weak_interface = 0;
        e->is_weak_interface_array = 0;
        e->weak_interface_array_size = 0;
        e->is_array = 0;
        e->array_elem_kind = 0;
        e->array_elem_size_expr[0] = '\0';
        e->is_struct_dtor = 0;
        e->struct_dtor_name[0] = '\0';
        ctx->cleanup_count++;
    }
}

static void cleanup_add_interface_array(CodegenContext* ctx, const char* name, int size) {
    if (ctx->cleanup_count < MAX_CLEANUP) {
        ctx->cleanup_entries[ctx->cleanup_count].name = name;
        ctx->cleanup_entries[ctx->cleanup_count].is_weak = 0;
        ctx->cleanup_entries[ctx->cleanup_count].is_interface = 0;
        ctx->cleanup_entries[ctx->cleanup_count].is_interface_array = 1;
        ctx->cleanup_entries[ctx->cleanup_count].interface_array_size = size;
        ctx->cleanup_entries[ctx->cleanup_count].is_weak_interface = 0;
        ctx->cleanup_entries[ctx->cleanup_count].is_weak_interface_array = 0;
        ctx->cleanup_entries[ctx->cleanup_count].weak_interface_array_size = 0;
        ctx->cleanup_entries[ctx->cleanup_count].is_array = 0;
        ctx->cleanup_entries[ctx->cleanup_count].array_elem_kind = 0;
        ctx->cleanup_entries[ctx->cleanup_count].array_elem_size_expr[0] = '\0';
        ctx->cleanup_entries[ctx->cleanup_count].is_struct_dtor = 0;
        ctx->cleanup_entries[ctx->cleanup_count].struct_dtor_name[0] = '\0';
        ctx->cleanup_count++;
    }
}

static void cleanup_add_array(CodegenContext* ctx, const char* name, const Type* arr_type) {
    if (ctx->cleanup_count < MAX_CLEANUP) {
        CleanupEntry* e = &ctx->cleanup_entries[ctx->cleanup_count];
        e->name = name;
        e->is_weak = 0;
        e->is_interface = 0;
        e->is_interface_array = 0;
        e->interface_array_size = 0;
        e->is_weak_interface = 0;
        e->is_weak_interface_array = 0;
        e->weak_interface_array_size = 0;
        e->is_array = 1;
        e->array_elem_kind = array_elem_kind(arr_type);
        array_elem_size_expr(arr_type, e->array_elem_size_expr, sizeof(e->array_elem_size_expr));
        e->is_struct_dtor = 0;
        e->struct_dtor_name[0] = '\0';
        ctx->cleanup_count++;
    }
}

static void cleanup_add_weak_interface(CodegenContext* ctx, const char* name) {
    if (ctx->cleanup_count < MAX_CLEANUP) {
        CleanupEntry* e = &ctx->cleanup_entries[ctx->cleanup_count];
        e->name = name;
        e->is_weak = 0;
        e->is_interface = 0;
        e->is_interface_array = 0;
        e->interface_array_size = 0;
        e->is_weak_interface = 1;
        e->is_weak_interface_array = 0;
        e->weak_interface_array_size = 0;
        e->is_array = 0;
        e->array_elem_kind = 0;
        e->array_elem_size_expr[0] = '\0';
        e->is_struct_dtor = 0;
        e->struct_dtor_name[0] = '\0';
        ctx->cleanup_count++;
    }
}

static void cleanup_add_weak_interface_array(CodegenContext* ctx, const char* name, int size) {
    if (ctx->cleanup_count < MAX_CLEANUP) {
        CleanupEntry* e = &ctx->cleanup_entries[ctx->cleanup_count];
        e->name = name;
        e->is_weak = 0;
        e->is_interface = 0;
        e->is_interface_array = 0;
        e->interface_array_size = 0;
        e->is_weak_interface = 0;
        e->is_weak_interface_array = 1;
        e->weak_interface_array_size = size;
        e->is_array = 0;
        e->array_elem_kind = 0;
        e->array_elem_size_expr[0] = '\0';
        e->is_struct_dtor = 0;
        e->struct_dtor_name[0] = '\0';
        ctx->cleanup_count++;
    }
}

/* Struct with reference fields: the compiler-generated release hook drops
   every owned share the struct holds (class/interface/weak fields). */
static void cleanup_add_struct_dtor(CodegenContext* ctx, const char* name, const char* struct_name) {
    if (ctx->cleanup_count < MAX_CLEANUP) {
        CleanupEntry* e = &ctx->cleanup_entries[ctx->cleanup_count];
        e->name = name;
        e->is_weak = 0;
        e->is_interface = 0;
        e->is_interface_array = 0;
        e->interface_array_size = 0;
        e->is_weak_interface = 0;
        e->is_weak_interface_array = 0;
        e->weak_interface_array_size = 0;
        e->is_array = 0;
        e->array_elem_kind = 0;
        e->array_elem_size_expr[0] = '\0';
        e->is_struct_dtor = 1;
        CHECK_STRSCPY(strscpy(e->struct_dtor_name, struct_name, sizeof(e->struct_dtor_name)),
                      "struct name too long");
        ctx->cleanup_count++;
    }
}

static void cleanup_emit(CodegenContext* ctx, int indent) {
    int i;
    for (i = ctx->cleanup_count - 1; i >= 0; i--) {
        const char* name = ctx->cleanup_entries[i].name;
        indent_line(ctx, indent);
        if (ctx->cleanup_entries[i].is_struct_dtor) {
            fprintf(ctx->out, "_mylang_release_%s(&%s);\n",
                    ctx->cleanup_entries[i].struct_dtor_name, name);
        } else if (ctx->cleanup_entries[i].is_weak) {
            fprintf(ctx->out, "mylang_weak_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface) {
            fprintf(ctx->out, "mylang_weak_release(%s.wr);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].weak_interface_array_size; j++) {
                fprintf(ctx->out, "mylang_weak_release(%s[%d].wr);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].interface_array_size; j++) {
                fprintf(ctx->out, "mylang_release(%s[%d].data);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface) {
            fprintf(ctx->out, "mylang_release(%s.data);\n", name);
        } else if (ctx->cleanup_entries[i].is_array) {
            fprintf(ctx->out, "mylang_array_free(&%s, %s, %d);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name,
                    ctx->cleanup_entries[i].array_elem_size_expr,
                    ctx->cleanup_entries[i].array_elem_kind);
        } else {
            fprintf(ctx->out, "mylang_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        }
    }
}

static void cleanup_push_scope(CodegenContext* ctx) {
    if (ctx->cleanup_scope_depth < MAX_SCOPE) {
        ctx->cleanup_scope_stack[ctx->cleanup_scope_depth++] = ctx->cleanup_count;
    }
}

static void cleanup_pop_scope(CodegenContext* ctx, int indent) {
    if (ctx->cleanup_scope_depth == 0) return;
    int saved = ctx->cleanup_scope_stack[--ctx->cleanup_scope_depth];
    int i;
    for (i = ctx->cleanup_count - 1; i >= saved; i--) {
        const char* name = ctx->cleanup_entries[i].name;
        indent_line(ctx, indent);
        if (ctx->cleanup_entries[i].is_struct_dtor) {
            fprintf(ctx->out, "_mylang_release_%s(&%s);\n",
                    ctx->cleanup_entries[i].struct_dtor_name, name);
        } else if (ctx->cleanup_entries[i].is_weak) {
            fprintf(ctx->out, "mylang_weak_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface) {
            fprintf(ctx->out, "mylang_weak_release(%s.wr);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].weak_interface_array_size; j++) {
                fprintf(ctx->out, "mylang_weak_release(%s[%d].wr);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].interface_array_size; j++) {
                fprintf(ctx->out, "mylang_release(%s[%d].data);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface) {
            fprintf(ctx->out, "mylang_release(%s.data);\n", name);
        } else if (ctx->cleanup_entries[i].is_array) {
            fprintf(ctx->out, "mylang_array_free(&%s, %s, %d);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name,
                    ctx->cleanup_entries[i].array_elem_size_expr,
                    ctx->cleanup_entries[i].array_elem_kind);
        } else {
            fprintf(ctx->out, "mylang_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        }
    }
    ctx->cleanup_count = saved;
}

static void cleanup_emit_to(CodegenContext* ctx, int target_count, int indent) {
    int i;
    for (i = ctx->cleanup_count - 1; i >= target_count; i--) {
        const char* name = ctx->cleanup_entries[i].name;
        indent_line(ctx, indent);
        if (ctx->cleanup_entries[i].is_struct_dtor) {
            fprintf(ctx->out, "_mylang_release_%s(&%s);\n",
                    ctx->cleanup_entries[i].struct_dtor_name, name);
        } else if (ctx->cleanup_entries[i].is_weak) {
            fprintf(ctx->out, "mylang_weak_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface) {
            fprintf(ctx->out, "mylang_weak_release(%s.wr);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].weak_interface_array_size; j++) {
                fprintf(ctx->out, "mylang_weak_release(%s[%d].wr);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].interface_array_size; j++) {
                fprintf(ctx->out, "mylang_release(%s[%d].data);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface) {
            fprintf(ctx->out, "mylang_release(%s.data);\n", name);
        } else if (ctx->cleanup_entries[i].is_array) {
            fprintf(ctx->out, "mylang_array_free(&%s, %s, %d);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name,
                    ctx->cleanup_entries[i].array_elem_size_expr,
                    ctx->cleanup_entries[i].array_elem_kind);
        } else {
            fprintf(ctx->out, "mylang_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        }
    }
}

static void codegen_match_stmt(CodegenContext* ctx, AstNode* node, int indent);
static void codegen_stmt(CodegenContext* ctx, AstNode* node, int indent);

static void indent_line(CodegenContext* ctx, int indent) {
    int i;
    for (i = 0; i < indent; i++) fprintf(ctx->out, "    ");
}

/* Prepare an expression for codegen: lower property accesses into accessor
   calls, fill default call arguments, lower any f-strings, then extract
   owned subexpression temporaries and caller-side guarded temporaries. */
static void prepare_expression(CodegenContext* ctx, AstNode* expr, int indent) {
    lower_property_access(ctx, expr);
    materialize_call_defaults_walk(expr);
    emit_fstring_preambles(ctx, expr, indent);
    emit_subexpr_temps(ctx, expr, indent);
    emit_guarded_temp_decls(ctx, expr, indent);
}

/* Prepare a loop condition expression.  Unlike prepare_expression, an owned
   root temporary (e.g. a call result used directly as the condition) is
   tracked on the cleanup list: the value is consumed by the condition test,
   not by an enclosing statement, so it must be released at the end of each
   iteration. */
static void prepare_condition(CodegenContext* ctx, AstNode* cond, int indent) {
    lower_property_access(ctx, cond);
    materialize_call_defaults_walk(cond);
    emit_fstring_preambles(ctx, cond, indent);
    emit_subexpr_temps(ctx, cond, indent);
    emit_guarded_temp_decls_impl(ctx, cond, indent, 0, 0);
}

/* Update the thread-local line marker when the source line changes. The file
   is already set by MY_PUSH at function entry, so only the line needs to be
   tracked. */
static void emit_line_loc(CodegenContext* ctx, AstNode* node, int indent) {
    if (!node) return;
    int line = node->ast_token.line;
    if (line <= 0 || line == ctx->last_loc_line) return;
    ctx->last_loc_line = line;
    indent_line(ctx, indent);
    fprintf(ctx->out, "MY_LOC(%d);\n", line);
}

static void codegen_body(CodegenContext* ctx, AstNode* body, int indent) {
    indent_line(ctx, indent);
    fprintf(ctx->out, "{\n");
    cleanup_push_scope(ctx);
    if (body->ast_kind == AST_BLOCK) {
        AstNode* s = body->ast_children[0];
        while (s) {
            codegen_stmt(ctx, s, indent + 1);
            s = s->next;
        }
    } else {
        codegen_stmt(ctx, body, indent + 1);
    }
    cleanup_pop_scope(ctx, indent + 1);
    indent_line(ctx, indent);
    fprintf(ctx->out, "}\n");
}

static void codegen_var_decl(CodegenContext* ctx, AstNode* node, int indent) {
    Type type = node->ast_resolved_type;

    /* Update the panic location before evaluating the initializer. */
    emit_line_loc(ctx, node, indent);

    if (node->ast_child_count > 0) {
        prepare_expression(ctx, node->ast_children[0], indent);
    }

    /* null may only initialize reference types (class, interface, weak). */
    if (node->ast_child_count > 0) {
        AstNode* init = node->ast_children[0];
        resolve_type(init);
        if (type_is_null(&init->ast_resolved_type) && !type_accepts_null(&type)) {
            if (type.is_unowned) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize unowned '%s' with 'null'", type.class_name);
            } else {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize '%s' with 'null'", type_name(&type));
            }
            ctx->codegen_error = 1;
            indent_line(ctx, indent);
            fprintf(ctx->out, "int %s = 0; /* invalid null initializer */\n",
                    node->ast_token.text);
            return;
        }
    }

    symtab_insert(node->ast_token.text, type);

    if (type.is_array) {
        if (type_is_ref_struct_array(&type)) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "arrays of struct '%s' with reference fields are not supported yet", type.class_name);
        }
        char typename_buf[128];
        c_type_str(&type, typename_buf, sizeof(typename_buf));
        indent_line(ctx, indent);
        fprintf(ctx->out, "%s %s", typename_buf, node->ast_token.text);
        if (node->ast_child_count > 0) {
            fprintf(ctx->out, " = ");
            codegen_expr(ctx, node->ast_children[0]);
        } else {
            fprintf(ctx->out, " = {0}");
        }
        fprintf(ctx->out, ";\n");
        cleanup_add_array(ctx, node->ast_token.text, &type);
        return;
    }

    if (type.is_weak && type.type_kind == TYPE_INTERFACE) {
        char winame[128];
        c_weak_interface_name(&type, winame, sizeof(winame));

        if (node->ast_child_count > 0) {
            AstNode* init = node->ast_children[0];
            resolve_type(init);
            Type rhs_type = init->ast_resolved_type;
            int rhs_owned = (expr_is_owned(init));

            indent_line(ctx, indent);
            fprintf(ctx->out, "%s %s;\n", winame, node->ast_token.text);

            if (rhs_type.is_weak && rhs_type.type_kind == TYPE_INTERFACE) {
                /* weak-to-weak copy */
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.wr = mylang_weak_copy(", node->ast_token.text);
                codegen_expr(ctx, init);
                fprintf(ctx->out, ".wr);\n");
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.vt = ", node->ast_token.text);
                codegen_expr(ctx, init);
                fprintf(ctx->out, ".vt;\n");
            } else if (rhs_type.type_kind == TYPE_INTERFACE) {
                /* strong interface -> weak interface */
                if (rhs_owned) {
                    int tmp_id = ctx->assign_tmp_id++;
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s _winit%d = ", type.class_name, tmp_id);
                    codegen_expr(ctx, init);
                    fprintf(ctx->out, ";\n");
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.wr = mylang_weak_init(_winit%d.data);\n",
                            node->ast_token.text, tmp_id);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.vt = _winit%d.vtable;\n",
                            node->ast_token.text, tmp_id);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "mylang_release(_winit%d.data);\n", tmp_id);
                } else {
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.wr = mylang_weak_init(", node->ast_token.text);
                    codegen_expr(ctx, init);
                    fprintf(ctx->out, ".data);\n");
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.vt = ", node->ast_token.text);
                    codegen_expr(ctx, init);
                    fprintf(ctx->out, ".vtable;\n");
                }
            } else if (rhs_type.type_kind == TYPE_CLASS) {
                /* class -> weak interface */
                if (rhs_owned) {
                    int tmp_id = ctx->assign_tmp_id++;
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "void* _winit%d = ", tmp_id);
                    codegen_expr(ctx, init);
                    fprintf(ctx->out, ";\n");
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.wr = mylang_weak_init(_winit%d);\n",
                            node->ast_token.text, tmp_id);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.vt = &%s_%s_vtable;\n",
                            node->ast_token.text,
                            rhs_type.class_name,
                            type.class_name);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "mylang_release(_winit%d);\n", tmp_id);
                } else {
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.wr = mylang_weak_init(", node->ast_token.text);
                    codegen_expr(ctx, init);
                    fprintf(ctx->out, ");\n");
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.vt = &%s_%s_vtable;\n",
                            node->ast_token.text,
                            rhs_type.class_name,
                            type.class_name);
                }
            } else if (type_is_null(&rhs_type)) {
                /* null -> weak interface */
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.wr = NULL;\n", node->ast_token.text);
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.vt = NULL;\n", node->ast_token.text);
            } else {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize weak interface '%s' with this value", type.class_name);
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s %s = { NULL, NULL };\n", winame, node->ast_token.text);
            }
        } else {
            indent_line(ctx, indent);
            fprintf(ctx->out, "%s %s = { NULL, NULL };\n", winame, node->ast_token.text);
        }
        cleanup_add_weak_interface(ctx, node->ast_token.text);
        return;
    }

    if (type.is_unowned && node->ast_child_count == 0) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "unowned variable '%s' requires an initializer", node->ast_token.text);
        indent_line(ctx, indent);
        fprintf(ctx->out, "WeakRef* %s = NULL;\n", node->ast_token.text);
        cleanup_add(ctx, node->ast_token.text, 1, 0);
        return;
    }

    if ((type.is_weak || type.is_unowned) && node->ast_child_count > 0) {
        AstNode* init = node->ast_children[0];
        resolve_type(init);
        Type it = init->ast_resolved_type;
        if (type_is_null(&it) && type.is_unowned) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize unowned '%s' with 'null'", type.class_name);
            indent_line(ctx, indent);
            fprintf(ctx->out, "WeakRef* %s = NULL;\n", node->ast_token.text);
            cleanup_add(ctx, node->ast_token.text, 1, 0);
            return;
        }
        if (it.type_kind != TYPE_CLASS && !type_is_null(&it)) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize '%s' with '%s'", type_name(&type), type_name(&it));
            indent_line(ctx, indent);
            fprintf(ctx->out, "WeakRef* %s = NULL;\n", node->ast_token.text);
            cleanup_add(ctx, node->ast_token.text, 1, 0);
            return;
        }
        if (it.is_weak || it.is_unowned) {
            indent_line(ctx, indent);
            fprintf(ctx->out, "WeakRef* %s = mylang_weak_copy(", node->ast_token.text);
            codegen_expr_raw(ctx, init);
            fprintf(ctx->out, ");\n");
        } else {
            int rhs_owned = (expr_is_owned(node->ast_children[0]));
            if (rhs_owned) {
                int tmp_id = ctx->assign_tmp_id++;
                indent_line(ctx, indent);
                fprintf(ctx->out, "void* _w%d = ", tmp_id);
                codegen_expr(ctx, node->ast_children[0]);
                fprintf(ctx->out, ";\n");
                indent_line(ctx, indent);
                fprintf(ctx->out, "WeakRef* %s = mylang_weak_init(_w%d);\n",
                        node->ast_token.text, tmp_id);
                indent_line(ctx, indent);
                fprintf(ctx->out, "mylang_release(_w%d);\n", tmp_id);
            } else {
                indent_line(ctx, indent);
                fprintf(ctx->out, "WeakRef* %s = mylang_weak_init(", node->ast_token.text);
                codegen_expr(ctx, node->ast_children[0]);
                fprintf(ctx->out, ");\n");
            }
        }
        cleanup_add(ctx, node->ast_token.text, 1, 0);
        return;
    }

    if (type.type_kind == TYPE_INTERFACE) {
        if (node->ast_child_count > 0) {
            AstNode* init = node->ast_children[0];
            resolve_type(init);
            int rhs_owned = (expr_is_owned(init));

            if (init->ast_resolved_type.type_kind == TYPE_CLASS) {
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s %s;\n", type.class_name, node->ast_token.text);
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.data = (void*)", node->ast_token.text);
                if (!rhs_owned) fprintf(ctx->out, "mylang_retain(");
                codegen_expr(ctx, init);
                if (!rhs_owned) fprintf(ctx->out, ")");
                fprintf(ctx->out, ";\n");
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.vtable = &%s_%s_vtable;\n",
                        node->ast_token.text,
                        init->ast_resolved_type.class_name,
                        type.class_name);
            } else if (init->ast_resolved_type.type_kind == TYPE_INTERFACE) {
                int tmp_id = ctx->assign_tmp_id++;
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s _init%d = ", type.class_name, tmp_id);
                codegen_expr(ctx, init);
                fprintf(ctx->out, ";\n");
                if (!rhs_owned) {
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "mylang_retain(_init%d.data);\n", tmp_id);
                }
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s %s = _init%d;\n", type.class_name, node->ast_token.text, tmp_id);
            } else if (type_is_null(&init->ast_resolved_type)) {
                /* null -> interface */
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s %s = { NULL, NULL };\n",
                        type.class_name, node->ast_token.text);
            } else {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize interface '%s' with non-class value", type.class_name);
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s %s = { NULL, NULL };\n", type.class_name, node->ast_token.text);
            }
        } else {
            indent_line(ctx, indent);
            fprintf(ctx->out, "%s %s = { NULL, NULL };\n", type.class_name, node->ast_token.text);
        }
        cleanup_add(ctx, node->ast_token.text, 0, 1);
        return;
    }

    indent_line(ctx, indent);

    {
        char typename_buf[128];
        c_type_str(&type, typename_buf, sizeof(typename_buf));
        fprintf(ctx->out, "%s %s", typename_buf, node->ast_token.text);
    }

    if (type.is_const && node->ast_child_count == 0) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "const variable '%s' requires an initializer", node->ast_token.text);
    }

    if (node->ast_child_count > 0) {
        AstNode* init = node->ast_children[0];
        resolve_type(init);
        Type it = init->ast_resolved_type;
        if (type_is_null(&it)) {
            /* Only reference types reach here with a null initializer;
               all other targets were rejected above. */
            fprintf(ctx->out, " = NULL");
        } else if (type.type_kind == TYPE_OBJECT) {
            /* object accepts any class, interface, or object value. */
            if (it.type_kind == TYPE_INTERFACE && !it.is_weak) {
                /* interface -> object: keep .data, drop the vtable */
                if (expr_is_owned(init)) {
                    fprintf(ctx->out, " = (void*)(");
                    codegen_expr(ctx, init);
                    fprintf(ctx->out, ").data");
                } else {
                    fprintf(ctx->out, " = mylang_retain((");
                    codegen_expr(ctx, init);
                    fprintf(ctx->out, ").data)");
                }
            } else if (it.type_kind == TYPE_CLASS || it.type_kind == TYPE_OBJECT) {
                if (expr_is_owned(init)) {
                    fprintf(ctx->out, " = ");
                    codegen_expr(ctx, init);
                } else {
                    fprintf(ctx->out, " = mylang_retain(");
                    codegen_expr(ctx, init);
                    fprintf(ctx->out, ")");
                }
            } else {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize 'object' with '%s'", type_name(&it));
                fprintf(ctx->out, " = NULL /* invalid object initializer */");
            }
        } else if (type.type_kind == TYPE_CLASS && it.type_kind == TYPE_OBJECT) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize '%s' with 'object'; cast with 'as' first", type_name(&type));
            fprintf(ctx->out, " = NULL /* invalid object initializer */");
        } else if (type.type_kind == TYPE_CLASS) {
            if (!expr_is_unowned_lock(init) && (it.type_kind != TYPE_CLASS || it.is_weak)) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize '%s' with '%s'", type_name(&type), type_name(&it));
                fprintf(ctx->out, " = NULL /* invalid class initializer */");
            } else if (!expr_is_owned(init)) {
                fprintf(ctx->out, " = mylang_retain(");
                codegen_expr(ctx, init);
                fprintf(ctx->out, ")");
            } else {
                fprintf(ctx->out, " = ");
                codegen_expr(ctx, init);
            }
        } else if (type.type_kind != TYPE_CLASS && type.type_kind != TYPE_INTERFACE &&
                   type.type_kind != TYPE_OBJECT &&
                   (bool_mismatch(&type, &it) || enum_mismatch(&type, &it))) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize '%s' with '%s'", type_name(&type), type_name(&it));
            fprintf(ctx->out, " = 0 /* invalid bool initializer */");
        } else if (type.type_kind != TYPE_CLASS && type.type_kind != TYPE_INTERFACE &&
                   type.type_kind != TYPE_OBJECT && type_is_reference(&it)) {
            codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "cannot initialize '%s' with '%s'", type_name(&type), type_name(&it));
            fprintf(ctx->out, " = 0 /* invalid reference initializer */");
        } else {
            fprintf(ctx->out, " = ");
            codegen_expr(ctx, init);
        }
    } else if (struct_has_ref_fields(&type)) {
        fprintf(ctx->out, " = {0}");
    } else if (type.is_pointer || type.is_weak) {
        fprintf(ctx->out, " = NULL");
    }
    fprintf(ctx->out, ";\n");

    /* Struct with reference fields: borrowed initializers are retained (call
       results are already owned); the variable is released at scope exit. */
    if (struct_has_ref_fields(&type)) {
        if (node->ast_child_count > 0 && !expr_is_owned(node->ast_children[0])) {
            indent_line(ctx, indent);
            fprintf(ctx->out, "_mylang_retain_%s(&%s);\n", type.class_name, node->ast_token.text);
        }
        cleanup_add_struct_dtor(ctx, node->ast_token.text, type.class_name);
    }

    if ((type.type_kind == TYPE_CLASS || type.type_kind == TYPE_OBJECT) &&
        type.is_pointer && !type.is_weak) {
        cleanup_add(ctx, node->ast_token.text, 0, 0);
    }
    if (type.is_weak) {
        cleanup_add(ctx, node->ast_token.text, 1, 0);
    }
}

static void codegen_if_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    /* Give the condition its own cleanup scope: an owned root temporary
       (e.g. a call result used directly as the condition) is consumed by the
       condition test and must be released when the if/else statement ends. */
    cleanup_push_scope(ctx);
    emit_line_loc(ctx, node->ast_children[0], indent);
    prepare_condition(ctx, node->ast_children[0], indent);
    emit_bounds_checks(ctx, node->ast_children[0], indent);
    indent_line(ctx, indent);
    fprintf(ctx->out, "if (");
    codegen_expr(ctx, node->ast_children[0]);
    fprintf(ctx->out, ")\n");
    codegen_body(ctx, node->ast_children[1], indent);

    if (node->ast_child_count > 2) {
        indent_line(ctx, indent);
        fprintf(ctx->out, "else\n");
        codegen_body(ctx, node->ast_children[2], indent);
    }
    cleanup_pop_scope(ctx, indent);
}

static void codegen_while_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    if (ctx->loop_depth >= MAX_LOOP) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                             "too many nested loops (max %d)", MAX_LOOP);
        return;
    }
    int break_lbl = ctx->assign_tmp_id++;
    int continue_lbl = ctx->assign_tmp_id++;
    ctx->loop_entry_cleanup_count[ctx->loop_depth] = ctx->cleanup_count;
    ctx->loop_break_label_id[ctx->loop_depth] = break_lbl;
    ctx->loop_continue_label_id[ctx->loop_depth] = continue_lbl;
    ctx->loop_depth++;

    indent_line(ctx, indent);
    fprintf(ctx->out, "while (1)\n");
    indent_line(ctx, indent);
    fprintf(ctx->out, "{\n");
    cleanup_push_scope(ctx);

    /* Evaluate the condition inside the loop: guarded temporaries (calls
       returning class/interface values) must be re-evaluated on every
       iteration and released at the end of it. */
    int cond_cleanup_base = ctx->cleanup_count;
    emit_line_loc(ctx, node->ast_children[0], indent + 1);
    prepare_condition(ctx, node->ast_children[0], indent + 1);
    emit_bounds_checks(ctx, node->ast_children[0], indent + 1);

    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "if (!(");
    codegen_expr(ctx, node->ast_children[0]);
    fprintf(ctx->out, "))\n");
    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "{\n");
    /* The false path skips the cleanup at the end of the loop body, so
       release this iteration's condition temporaries before leaving. */
    cleanup_emit_to(ctx, cond_cleanup_base, indent + 2);
    indent_line(ctx, indent + 2);
    fprintf(ctx->out, "goto _my_break%d;\n", break_lbl);
    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "}\n");

    codegen_body(ctx, node->ast_children[1], indent + 1);

    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "_my_continue%d:;\n", continue_lbl);

    cleanup_pop_scope(ctx, indent + 1);
    indent_line(ctx, indent);
    fprintf(ctx->out, "}\n");
    indent_line(ctx, indent);
    fprintf(ctx->out, "_my_break%d:;\n", break_lbl);

    ctx->loop_depth--;
}

static void codegen_for_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    AstNode* init = (node->ast_child_count > 0) ? node->ast_children[0] : NULL;
    AstNode* cond = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
    AstNode* step = (node->ast_child_count > 2) ? node->ast_children[2] : NULL;
    AstNode* body = (node->ast_child_count > 3) ? node->ast_children[3] : NULL;

    indent_line(ctx, indent);
    fprintf(ctx->out, "{\n");
    cleanup_push_scope(ctx);

    if (init) {
        if (init->ast_kind == AST_VAR_DECL) {
            codegen_var_decl(ctx, init, indent + 1);
        } else {
            AstNode es;
            memset(&es, 0, sizeof(es));
            es.ast_kind = AST_EXPR_STMT;
            es.ast_token = init->ast_token;
            es.ast_children[0] = init;
            es.ast_child_count = 1;
            codegen_expr_stmt(ctx, &es, indent + 1);
        }
    }

    if (ctx->loop_depth >= MAX_LOOP) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                             "too many nested loops (max %d)", MAX_LOOP);
        cleanup_pop_scope(ctx, indent + 1);
        indent_line(ctx, indent);
        fprintf(ctx->out, "}\n");
        return;
    }
    int break_lbl = ctx->assign_tmp_id++;
    int continue_lbl = ctx->assign_tmp_id++;
    ctx->loop_entry_cleanup_count[ctx->loop_depth] = ctx->cleanup_count;
    ctx->loop_break_label_id[ctx->loop_depth] = break_lbl;
    ctx->loop_continue_label_id[ctx->loop_depth] = continue_lbl;
    ctx->loop_depth++;

    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "while (1)\n");
    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "{\n");
    cleanup_push_scope(ctx);

    if (cond) {
        /* Evaluate the condition inside the loop: guarded temporaries (calls
           returning class/interface values) must be re-evaluated on every
           iteration and released at the end of it. */
        int cond_cleanup_base = ctx->cleanup_count;
        emit_line_loc(ctx, cond, indent + 2);
        prepare_condition(ctx, cond, indent + 2);
        emit_bounds_checks(ctx, cond, indent + 2);

        indent_line(ctx, indent + 2);
        fprintf(ctx->out, "if (!(");
        codegen_expr(ctx, cond);
        fprintf(ctx->out, "))\n");
        indent_line(ctx, indent + 2);
        fprintf(ctx->out, "{\n");
        /* The false path skips the cleanup at the end of the loop body, so
           release this iteration's condition temporaries before leaving. */
        cleanup_emit_to(ctx, cond_cleanup_base, indent + 3);
        indent_line(ctx, indent + 3);
        fprintf(ctx->out, "goto _my_break%d;\n", break_lbl);
        indent_line(ctx, indent + 2);
        fprintf(ctx->out, "}\n");
    }

    if (body) {
        if (body->ast_kind == AST_BLOCK) {
            AstNode* s = body->ast_children[0];
            while (s) {
                codegen_stmt(ctx, s, indent + 2);
                s = s->next;
            }
        } else {
            codegen_stmt(ctx, body, indent + 2);
        }
    }

    indent_line(ctx, indent + 2);
    fprintf(ctx->out, "_my_continue%d:;\n", continue_lbl);

    if (step) {
        AstNode es;
        memset(&es, 0, sizeof(es));
        es.ast_kind = AST_EXPR_STMT;
        es.ast_token = step->ast_token;
        es.ast_children[0] = step;
        es.ast_child_count = 1;
        codegen_expr_stmt(ctx, &es, indent + 2);
    }

    cleanup_pop_scope(ctx, indent + 2);
    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "}\n");

    ctx->loop_depth--;
    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "_my_break%d:;\n", break_lbl);

    cleanup_pop_scope(ctx, indent + 1);
    indent_line(ctx, indent);
    fprintf(ctx->out, "}\n");
}

/* foreach (T x in arr) { body } - lowered to an index loop:
     {
         u64 _feN = 0;
         while (1) {
             if (!(_feN < arr.length())) goto _my_breakB;
             T x = arr[_feN];
             ... body ...
             _my_continueC:;
             _feN = _feN + 1;
         }
         _my_breakB:;
     }
   The loop variable is a per-iteration copy (a normal local, so class
   elements are retained/released by the usual cleanup).  The length is
   re-read on every iteration ("live" length): mutating the array inside
   the body affects iteration, and the bounds check in mylang_array_at
   keeps that memory-safe. */
static void codegen_foreach_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    AstNode* decl = (node->ast_child_count > 0) ? node->ast_children[0] : NULL;
    AstNode* arr  = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
    AstNode* body = (node->ast_child_count > 2) ? node->ast_children[2] : NULL;
    if (!decl || !arr) return;

    resolve_type(arr);
    if (!arr->ast_resolved_type.is_array) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                             "foreach requires an array, got '%s'",
                             type_name(&arr->ast_resolved_type));
        return;
    }
    if (ctx->loop_depth >= MAX_LOOP) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                             "too many nested loops (max %d)", MAX_LOOP);
        return;
    }

    int break_lbl = ctx->assign_tmp_id++;
    int continue_lbl = ctx->assign_tmp_id++;
    char idx_name[32];
    int n = snprintf(idx_name, sizeof(idx_name), "_fe%d", ctx->assign_tmp_id++);
    CHECK_SNPRINTF(n, sizeof(idx_name), "foreach index name too long");

    /* Synthetic token shared by all generated nodes; it points at the
       'foreach' keyword for diagnostics. */
    Token fe_tok;
    memset(&fe_tok, 0, sizeof(fe_tok));
    fe_tok.kind = TOK_IDENT;
    strscpy(fe_tok.text, idx_name, sizeof(fe_tok.text));
    fe_tok.line = node->ast_token.line;
    fe_tok.col = node->ast_token.col;
    fe_tok.filename = node->ast_token.filename;

    /* u64 _feN = 0; - a real local so the identifier resolves everywhere. */
    AstNode idx_init;
    memset(&idx_init, 0, sizeof(idx_init));
    idx_init.ast_kind = AST_INT_LIT;
    idx_init.ast_token = fe_tok;
    idx_init.ast_token.kind = TOK_INT_LIT;
    strscpy(idx_init.ast_token.text, "0", sizeof(idx_init.ast_token.text));

    AstNode idx_decl;
    memset(&idx_decl, 0, sizeof(idx_decl));
    idx_decl.ast_kind = AST_VAR_DECL;
    idx_decl.ast_token = fe_tok;
    idx_decl.ast_resolved_type = type_make_primitive(TYPE_U64);
    idx_decl.ast_children[0] = &idx_init;
    idx_decl.ast_child_count = 1;

    AstNode idx_ident;
    memset(&idx_ident, 0, sizeof(idx_ident));
    idx_ident.ast_kind = AST_IDENT;
    idx_ident.ast_token = fe_tok;

    /* _feN < arr.length() */
    Token len_tok = fe_tok;
    strscpy(len_tok.text, "length", sizeof(len_tok.text));

    AstNode len_mem;
    memset(&len_mem, 0, sizeof(len_mem));
    len_mem.ast_kind = AST_MEMBER_ACCESS;
    len_mem.ast_token = len_tok;
    len_mem.ast_children[0] = arr;
    len_mem.ast_child_count = 1;

    AstNode len_call;
    memset(&len_call, 0, sizeof(len_call));
    len_call.ast_kind = AST_CALL;
    len_call.ast_token = len_tok;
    len_call.ast_children[0] = &len_mem;
    len_call.ast_child_count = 1;

    Token lt_tok = fe_tok;
    lt_tok.kind = TOK_LT;
    strscpy(lt_tok.text, "<", sizeof(lt_tok.text));

    AstNode cond;
    memset(&cond, 0, sizeof(cond));
    cond.ast_kind = AST_BINARY;
    cond.ast_token = lt_tok;
    cond.ast_children[0] = &idx_ident;
    cond.ast_children[1] = &len_call;
    cond.ast_child_count = 2;

    /* T x = arr[_feN]; - clone the collection expression so the element
       access has its own node state (temps, resolved type). */
    AstNode access;
    memset(&access, 0, sizeof(access));
    access.ast_kind = AST_ARRAY_ACCESS;
    access.ast_token = arr->ast_token;
    access.ast_children[0] = ast_clone(arr);
    access.ast_children[1] = &idx_ident;
    access.ast_child_count = 2;

    AstNode elem_decl = *decl;
    elem_decl.ast_children[0] = &access;
    elem_decl.ast_child_count = 1;
    elem_decl.next = NULL;

    ctx->loop_entry_cleanup_count[ctx->loop_depth] = ctx->cleanup_count;
    ctx->loop_break_label_id[ctx->loop_depth] = break_lbl;
    ctx->loop_continue_label_id[ctx->loop_depth] = continue_lbl;
    ctx->loop_depth++;

    indent_line(ctx, indent);
    fprintf(ctx->out, "{\n");
    cleanup_push_scope(ctx);
    symtab_enter_scope();

    codegen_var_decl(ctx, &idx_decl, indent + 1);

    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "while (1)\n");
    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "{\n");
    cleanup_push_scope(ctx);

    /* Same pattern as codegen_for_stmt: prepare per-iteration, release the
       condition's temporaries on the exit path. */
    int cond_cleanup_base = ctx->cleanup_count;
    emit_line_loc(ctx, &cond, indent + 2);
    prepare_condition(ctx, &cond, indent + 2);
    emit_bounds_checks(ctx, &cond, indent + 2);

    indent_line(ctx, indent + 2);
    fprintf(ctx->out, "if (!(");
    codegen_expr(ctx, &cond);
    fprintf(ctx->out, "))\n");
    indent_line(ctx, indent + 2);
    fprintf(ctx->out, "{\n");
    cleanup_emit_to(ctx, cond_cleanup_base, indent + 3);
    indent_line(ctx, indent + 3);
    fprintf(ctx->out, "goto _my_break%d;\n", break_lbl);
    indent_line(ctx, indent + 2);
    fprintf(ctx->out, "}\n");

    codegen_var_decl(ctx, &elem_decl, indent + 2);

    if (body) {
        if (body->ast_kind == AST_BLOCK) {
            AstNode* s = body->ast_children[0];
            while (s) {
                codegen_stmt(ctx, s, indent + 2);
                s = s->next;
            }
        } else {
            codegen_stmt(ctx, body, indent + 2);
        }
    }

    indent_line(ctx, indent + 2);
    fprintf(ctx->out, "_my_continue%d:;\n", continue_lbl);
    indent_line(ctx, indent + 2);
    fprintf(ctx->out, "%s = %s + 1;\n", idx_name, idx_name);

    cleanup_pop_scope(ctx, indent + 2);
    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "}\n");

    ctx->loop_depth--;
    indent_line(ctx, indent + 1);
    fprintf(ctx->out, "_my_break%d:;\n", break_lbl);

    symtab_exit_scope();
    cleanup_pop_scope(ctx, indent + 1);
    indent_line(ctx, indent);
    fprintf(ctx->out, "}\n");
}

static void codegen_break_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    (void)node;
    if (ctx->loop_depth == 0) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "'break' outside of loop");
        return;
    }
    int idx = ctx->loop_depth - 1;
    if (idx >= MAX_LOOP) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                             "'break' in loop beyond max nesting depth");
        return;
    }
    int target = ctx->loop_entry_cleanup_count[idx];
    cleanup_emit_to(ctx, target, indent);
    indent_line(ctx, indent);
    fprintf(ctx->out, "goto _my_break%d;\n", ctx->loop_break_label_id[idx]);
}

static void codegen_continue_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    (void)node;
    if (ctx->loop_depth == 0) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "'continue' outside of loop");
        return;
    }
    int idx = ctx->loop_depth - 1;
    if (idx >= MAX_LOOP) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col,
                             "'continue' in loop beyond max nesting depth");
        return;
    }
    int target = ctx->loop_entry_cleanup_count[idx];
    cleanup_emit_to(ctx, target, indent);
    indent_line(ctx, indent);
    fprintf(ctx->out, "goto _my_continue%d;\n", ctx->loop_continue_label_id[idx]);
}

static void codegen_match_arm_body(CodegenContext* ctx, AstNode* arm, AstNode* body,
                                    const char* temp_name, Type* expr_type, int indent) {
    indent_line(ctx, indent);
    fprintf(ctx->out, "{\n");
    cleanup_push_scope(ctx);
    symtab_enter_scope();

    Type pat_type = arm->ast_resolved_type;
    if (pat_type.type_kind == TYPE_CLASS) {
        char pat_cbuf[128];
        c_type_str(&pat_type, pat_cbuf, sizeof(pat_cbuf));
        indent_line(ctx, indent + 1);
        fprintf(ctx->out, "%s %s = ", pat_cbuf, arm->ast_token.text);
        if (expr_type->type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "(%s*)(%s.data);\n", pat_type.class_name, temp_name);
        } else {
            fprintf(ctx->out, "(%s*)(%s);\n", pat_type.class_name, temp_name);
        }
        symtab_insert(arm->ast_token.text, pat_type);
    }

    if (body && body->ast_kind == AST_BLOCK) {
        AstNode* s = body->ast_children[0];
        while (s) {
            codegen_stmt(ctx, s, indent + 1);
            s = s->next;
        }
    } else {
        codegen_stmt(ctx, body, indent + 1);
    }

    symtab_exit_scope();
    cleanup_pop_scope(ctx, indent + 1);
    indent_line(ctx, indent);
    fprintf(ctx->out, "}\n");
}

static void codegen_match_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    AstNode* expr = node->ast_children[0];
    AstNode* arms = node->ast_children[1];

    emit_line_loc(ctx, node, indent);
    if (expr) {
        prepare_expression(ctx, expr, indent);
        emit_bounds_checks(ctx, expr, indent);
    }

    Type expr_type;
    memset(&expr_type, 0, sizeof(expr_type));
    if (expr) {
        expr_type = resolve_type(expr);
    }
    if (expr && expr_type.is_unowned) {
        codegen_report_error(ctx, expr->ast_token.line, expr->ast_token.col, "cannot match on an unowned reference; convert it to a strong reference first");
    }
    if (expr && type_is_null(&expr_type)) {
        codegen_report_error(ctx, expr->ast_token.line, expr->ast_token.col, "cannot match on null");
    }

    int expr_owned = expr && expr_is_owned(expr);
    int expr_extracted = expr && (expr->ast_temp_name[0] != '\0');

    int tmp_id = ctx->assign_tmp_id++;
    CHECK_STRSCPY(strscpy(node->ast_temp_name, "_m", sizeof(node->ast_temp_name)),
                  "match temp name too long");
    {
        char suffix[32];
        int n = snprintf(suffix, sizeof(suffix), "%d", tmp_id);
        CHECK_SNPRINTF(n, sizeof(suffix), "match temp id too long");
        CHECK_STRSCPY(strscpy(node->ast_temp_name + 2, suffix, sizeof(node->ast_temp_name) - 2),
                      "match temp name too long");
    }
    const char* temp_name = node->ast_temp_name;

    char tbuf[128];
    c_type_str(&expr_type, tbuf, sizeof(tbuf));
    indent_line(ctx, indent);
    fprintf(ctx->out, "%s %s = ", tbuf, temp_name);
    if (expr) {
        codegen_expr(ctx, expr);
    } else {
        fprintf(ctx->out, "0");
    }
    fprintf(ctx->out, ";\n");

    /* If the expression is owned and was not hoisted into a subexpression
       temporary, this local holds the only reference and must be released. */
    if (expr_owned && !expr_extracted) {
        if (expr_type.type_kind == TYPE_INTERFACE) {
            cleanup_add(ctx, temp_name, 0, 1);
        } else if (expr_type.type_kind == TYPE_CLASS ||
                   expr_type.type_kind == TYPE_OBJECT) {
            cleanup_add(ctx, temp_name, 0, 0);
        }
    }

    int is_first = 1;
    AstNode* arm = arms;
    while (arm) {
        Type pat_type = arm->ast_resolved_type;

        if (pat_type.type_kind == TYPE_VOID) {
            /* else arm */
            if (is_first) {
                codegen_match_arm_body(ctx, arm, arm->ast_children[0], temp_name, &expr_type, indent);
            } else {
                indent_line(ctx, indent);
                fprintf(ctx->out, "else\n");
                codegen_match_arm_body(ctx, arm, arm->ast_children[0], temp_name, &expr_type, indent);
            }
            break;
        }

        if (pat_type.type_kind == TYPE_I32) {
            if (expr_type.type_kind != TYPE_I32 &&
                expr_type.type_kind != TYPE_I8 &&
                expr_type.type_kind != TYPE_I16 &&
                expr_type.type_kind != TYPE_I64 &&
                expr_type.type_kind != TYPE_U8 &&
                expr_type.type_kind != TYPE_U16 &&
                expr_type.type_kind != TYPE_U32 &&
                expr_type.type_kind != TYPE_U64) {
                codegen_report_error(ctx, arm->ast_token.line, arm->ast_token.col, "integer match pattern cannot match expression of type '%s'", type_name(&expr_type));
            }
            indent_line(ctx, indent);
            if (is_first) {
                fprintf(ctx->out, "if (%s == %d)\n", temp_name, arm->ast_token.int_val);
            } else {
                fprintf(ctx->out, "else if (%s == %d)\n", temp_name, arm->ast_token.int_val);
            }
            codegen_match_arm_body(ctx, arm, arm->ast_children[0], temp_name, &expr_type, indent);
        } else if (pat_type.type_kind == TYPE_ENUM) {
            /* Enum variant constant arm: compare against the C constant.  No
               exhaustiveness check; missing variants fall through. */
            if (expr_type.type_kind != TYPE_ENUM ||
                strcmp(expr_type.class_name, pat_type.class_name) != 0) {
                codegen_report_error(ctx, arm->ast_token.line, arm->ast_token.col,
                                     "match pattern type '%s' does not match expression type '%s'",
                                     type_name(&pat_type), type_name(&expr_type));
            }
            EnumInfo* ei = symtab_find_enum(pat_type.class_name);
            int found = 0;
            int vi;
            for (vi = 0; ei && vi < ei->variant_count; vi++) {
                if (strcmp(ei->variant_names[vi], arm->ast_token.text) == 0) { found = 1; break; }
            }
            if (!found) {
                codegen_report_error(ctx, arm->ast_token.line, arm->ast_token.col,
                                     "enum '%s' has no variant '%s'",
                                     pat_type.class_name, arm->ast_token.text);
            }
            indent_line(ctx, indent);
            if (is_first) {
                fprintf(ctx->out, "if (%s == %s_%s)\n",
                        temp_name, pat_type.class_name, arm->ast_token.text);
            } else {
                fprintf(ctx->out, "else if (%s == %s_%s)\n",
                        temp_name, pat_type.class_name, arm->ast_token.text);
            }
            codegen_match_arm_body(ctx, arm, arm->ast_children[0], temp_name, &expr_type, indent);
        } else if (pat_type.type_kind == TYPE_CLASS) {
            if (expr_type.type_kind == TYPE_INTERFACE) {
                ClassInfo* cls = symtab_find_class(pat_type.class_name);
                InterfaceInfo* iface = symtab_find_interface(expr_type.class_name);
                int implements = 0;
                if (cls && iface) {
                    int i;
                    for (i = 0; i < cls->impl_count && i < MAX_IMPL; i++) {
                        if (strcmp(cls->impl_names[i], expr_type.class_name) == 0) {
                            implements = 1;
                            break;
                        }
                    }
                }
                if (!implements) {
                    codegen_report_error(ctx, arm->ast_token.line, arm->ast_token.col, "class '%s' does not implement interface '%s'", pat_type.class_name, expr_type.class_name);
                }
                indent_line(ctx, indent);
                if (is_first) {
                    fprintf(ctx->out, "if (%s.vtable->concrete_type_id == MYLANG_TID_%s)\n",
                            temp_name, pat_type.class_name);
                } else {
                    fprintf(ctx->out, "else if (%s.vtable->concrete_type_id == MYLANG_TID_%s)\n",
                            temp_name, pat_type.class_name);
                }
            } else if (expr_type.type_kind == TYPE_CLASS ||
                       expr_type.type_kind == TYPE_OBJECT) {
                /* object matches any class pattern through the concrete type
                   id in the object header; a plain class expression keeps the
                   exact-name requirement. */
                if (expr_type.type_kind == TYPE_CLASS &&
                    strcmp(expr_type.class_name, pat_type.class_name) != 0) {
                    codegen_report_error(ctx, arm->ast_token.line, arm->ast_token.col, "match pattern type '%s' does not match expression type '%s'", pat_type.class_name, expr_type.class_name);
                }
                indent_line(ctx, indent);
                if (is_first) {
                    fprintf(ctx->out, "if (%s != NULL && mylang_obj_hdr(%s)->type_id == MYLANG_TID_%s)\n",
                            temp_name, temp_name, pat_type.class_name);
                } else {
                    fprintf(ctx->out, "else if (%s != NULL && mylang_obj_hdr(%s)->type_id == MYLANG_TID_%s)\n",
                            temp_name, temp_name, pat_type.class_name);
                }
            } else {
                codegen_report_error(ctx, arm->ast_token.line, arm->ast_token.col, "class match pattern cannot match expression of type '%s'", type_name(&expr_type));
                indent_line(ctx, indent);
                if (is_first) {
                    fprintf(ctx->out, "if (0)\n");
                } else {
                    fprintf(ctx->out, "else if (0)\n");
                }
            }
            codegen_match_arm_body(ctx, arm, arm->ast_children[0], temp_name, &expr_type, indent);
        } else {
            codegen_report_error(ctx, arm->ast_token.line, arm->ast_token.col, "unsupported match pattern type");
            indent_line(ctx, indent);
            if (is_first) {
                fprintf(ctx->out, "if (0)\n");
            } else {
                fprintf(ctx->out, "else if (0)\n");
            }
            codegen_match_arm_body(ctx, arm, arm->ast_children[0], temp_name, &expr_type, indent);
        }

        is_first = 0;
        arm = arm->next;
    }
}

static void codegen_return_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    if (node->ast_child_count > 0) {
        AstNode* ret = node->ast_children[0];
        emit_line_loc(ctx, ret, indent);
        prepare_expression(ctx, ret, indent);
        emit_bounds_checks(ctx, ret, indent);
        resolve_type(ret);

        if (type_is_null(&ret->ast_resolved_type)) {
            /* return null: only reference return types are allowed. */
            if (!type_accepts_null(&ctx->return_type)) {
                codegen_report_error(ctx, ret->ast_token.line, ret->ast_token.col, "cannot return null from function returning '%s'", type_name(&ctx->return_type));
            }
            if (ctx->return_type.type_kind == TYPE_INTERFACE) {
                char tbuf[128];
                c_type_str(&ctx->return_type, tbuf, sizeof(tbuf));
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s _mylang_ret = { NULL, NULL };\n", tbuf);
                cleanup_emit(ctx, indent);
                indent_line(ctx, indent);
                fprintf(ctx->out, "MY_POP();\n");
                indent_line(ctx, indent);
                fprintf(ctx->out, "return _mylang_ret;\n");
            } else {
                cleanup_emit(ctx, indent);
                indent_line(ctx, indent);
                fprintf(ctx->out, "MY_POP();\n");
                indent_line(ctx, indent);
                fprintf(ctx->out, "return NULL;\n");
            }
            return;
        }

        /* Strict bool/enum rules at the return boundary. */
        if (ctx->return_type.type_kind != TYPE_VOID &&
            (bool_mismatch(&ctx->return_type, &ret->ast_resolved_type) ||
             enum_mismatch(&ctx->return_type, &ret->ast_resolved_type))) {
            codegen_report_error(ctx, ret->ast_token.line, ret->ast_token.col, "cannot return '%s' from function returning '%s'", type_name(&ret->ast_resolved_type), type_name(&ctx->return_type));
        }

        /* object converts back to a concrete type only through 'as'. */
        if (ret->ast_resolved_type.type_kind == TYPE_OBJECT &&
            ctx->return_type.type_kind != TYPE_OBJECT) {
            codegen_report_error(ctx, ret->ast_token.line, ret->ast_token.col, "cannot return 'object' from function returning '%s'; cast with 'as' first", type_name(&ctx->return_type));
        }

        if (ret->ast_resolved_type.type_kind == TYPE_CLASS &&
            ctx->return_type.type_kind == TYPE_INTERFACE) {
            /* implicit class-to-interface conversion in return */
            int needs_retain = return_expr_needs_retain(ret);
            int tid = ctx->assign_tmp_id++;
            indent_line(ctx, indent);
            if (needs_retain) {
                fprintf(ctx->out, "void* _r = mylang_retain(");
            } else {
                fprintf(ctx->out, "void* _r = (");
            }
            codegen_expr(ctx, ret);
            fprintf(ctx->out, ");\n");
            indent_line(ctx, indent);
            fprintf(ctx->out, "%s _iret%d;\n", ctx->return_type.class_name, tid);
            indent_line(ctx, indent);
            fprintf(ctx->out, "_iret%d.data = _r;\n", tid);
            indent_line(ctx, indent);
            fprintf(ctx->out, "_iret%d.vtable = &%s_%s_vtable;\n",
                    tid, ret->ast_resolved_type.class_name, ctx->return_type.class_name);
            cleanup_emit(ctx, indent);
            indent_line(ctx, indent);
            fprintf(ctx->out, "MY_POP();\n");
            indent_line(ctx, indent);
            fprintf(ctx->out, "return _iret%d;\n", tid);
        } else if (ret->ast_resolved_type.type_kind == TYPE_CLASS ||
                   ret->ast_resolved_type.type_kind == TYPE_OBJECT) {
            int needs_retain = return_expr_needs_retain(ret);
            indent_line(ctx, indent);
            if (needs_retain) {
                fprintf(ctx->out, "void* _r = mylang_retain(");
            } else {
                fprintf(ctx->out, "void* _r = (");
            }
            codegen_expr(ctx, ret);
            fprintf(ctx->out, ");\n");
            cleanup_emit(ctx, indent);
            indent_line(ctx, indent);
            fprintf(ctx->out, "MY_POP();\n");
            indent_line(ctx, indent);
            fprintf(ctx->out, "return _r;\n");
        } else if (ret->ast_resolved_type.type_kind == TYPE_INTERFACE &&
                   ctx->return_type.type_kind == TYPE_OBJECT) {
            /* interface -> object in return: keep .data, drop the vtable */
            int needs_retain = return_expr_needs_retain(ret);
            indent_line(ctx, indent);
            if (needs_retain) {
                fprintf(ctx->out, "void* _r = mylang_retain((");
            } else {
                fprintf(ctx->out, "void* _r = (void*)(");
            }
            codegen_expr(ctx, ret);
            fprintf(ctx->out, ").data);\n");
            cleanup_emit(ctx, indent);
            indent_line(ctx, indent);
            fprintf(ctx->out, "MY_POP();\n");
            indent_line(ctx, indent);
            fprintf(ctx->out, "return _r;\n");
        } else if (ret->ast_resolved_type.is_array) {
            codegen_report_error(ctx, ret->ast_token.line, ret->ast_token.col, "cannot return array by value; use move_to(ref) through a ref parameter");
            indent_line(ctx, indent);
            fprintf(ctx->out, "/* invalid array return */\n");
            cleanup_emit(ctx, indent);
            indent_line(ctx, indent);
            fprintf(ctx->out, "MY_POP();\n");
            indent_line(ctx, indent);
            fprintf(ctx->out, "return;\n");
        } else if (ret->ast_resolved_type.type_kind == TYPE_INTERFACE) {
            int needs_retain = return_expr_needs_retain(ret);
            char tbuf[128];
            c_type_str(&ret->ast_resolved_type, tbuf, sizeof(tbuf));
            int tid = ctx->assign_tmp_id++;
            indent_line(ctx, indent);
            fprintf(ctx->out, "%s _iret%d = ", tbuf, tid);
            codegen_expr(ctx, ret);
            fprintf(ctx->out, ";\n");
            if (needs_retain) {
                indent_line(ctx, indent);
                fprintf(ctx->out, "mylang_retain(_iret%d.data);\n", tid);
            }
            cleanup_emit(ctx, indent);
            indent_line(ctx, indent);
            fprintf(ctx->out, "MY_POP();\n");
            indent_line(ctx, indent);
            fprintf(ctx->out, "return _iret%d;\n", tid);
        } else {
            char tbuf[128];
            c_type_str(&ret->ast_resolved_type, tbuf, sizeof(tbuf));
            indent_line(ctx, indent);
            fprintf(ctx->out, "%s _mylang_ret = ", tbuf);
            codegen_expr(ctx, ret);
            fprintf(ctx->out, ";\n");
            /* Struct with reference fields returning a borrowed value: retain
               its shares for the caller before the local cleanup releases
               the callee's own.  Call results are already owned. */
            if (struct_has_ref_fields(&ret->ast_resolved_type) && !expr_is_owned(ret)) {
                indent_line(ctx, indent);
                fprintf(ctx->out, "_mylang_retain_%s(&_mylang_ret);\n", ret->ast_resolved_type.class_name);
            }
            cleanup_emit(ctx, indent);
            indent_line(ctx, indent);
            fprintf(ctx->out, "MY_POP();\n");
            indent_line(ctx, indent);
            fprintf(ctx->out, "return _mylang_ret;\n");
        }
    } else {
        cleanup_emit(ctx, indent);
        indent_line(ctx, indent);
        fprintf(ctx->out, "MY_POP();\n");
        indent_line(ctx, indent);
        fprintf(ctx->out, "return;\n");
    }
}

static int is_compound_assign_op(TokenKind k) {
    return k == TOK_PLUS_ASSIGN || k == TOK_MINUS_ASSIGN ||
           k == TOK_STAR_ASSIGN || k == TOK_SLASH_ASSIGN ||
           k == TOK_AMP_ASSIGN || k == TOK_PIPE_ASSIGN || k == TOK_CARET_ASSIGN ||
           k == TOK_SHL_ASSIGN || k == TOK_SHR_ASSIGN;
}

/* For compound assignments and increment/decrement on non-trivial lvalues
   (array elements, member accesses, etc.), evaluate the address once into a
   temporary pointer so the left-hand side is not computed twice. */
static void emit_compound_lvalue_temp(CodegenContext* ctx, AstNode* expr, int indent) {
    AstNode* target = NULL;
    if (expr->ast_kind == AST_ASSIGN && is_compound_assign_op(expr->ast_token.kind)) {
        target = expr->ast_children[0];
    } else if (expr->ast_kind == AST_INC_DEC) {
        target = expr->ast_children[0];
    }
    if (!target || target->ast_kind == AST_IDENT) return;

    Type t = resolve_type(target);
    char typename_buf[128];
    c_type_str(&t, typename_buf, sizeof(typename_buf));
    int id = ctx->assign_tmp_id++;

    indent_line(ctx, indent);
    fprintf(ctx->out, "%s* _mylang_ca%d = &", typename_buf, id);
    codegen_expr(ctx, target);
    fprintf(ctx->out, ";\n");

    int n = snprintf(target->ast_temp_name, sizeof(target->ast_temp_name), "(*_mylang_ca%d)", id);
    CHECK_SNPRINTF(n, sizeof(target->ast_temp_name), "compound assignment temp name too long");
}

static void codegen_expr_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    emit_bounds_checks(ctx, node->ast_children[0], indent);
    AstNode* expr = node->ast_children[0];
    resolve_type(expr);

    /* Update the panic location before any runtime call in this statement. */
    emit_line_loc(ctx, expr, indent);

    /* Prepare the expression: lower f-strings, extract owned subexpressions,
       and extract guarded class subexpressions. */
    prepare_expression(ctx, expr, indent);

    /* class/array assignment: evaluate RHS first, release old LHS, then assign.
       This avoids use-after-free when RHS aliases LHS (e.g. b = b.set(5)). */
    if (expr->ast_kind == AST_ASSIGN) {
        AstNode* lhs = expr->ast_children[0];
        AstNode* rhs = expr->ast_children[1];
        resolve_type(lhs);
        resolve_type(rhs);
        Type lt = lhs->ast_resolved_type;

        {
            TokenKind assign_op = expr->ast_token.kind;
            if (is_compound_assign_op(assign_op)) {
                /* Compound assignment: arithmetic ops accept primitive
                   numeric types; bitwise ops require integer types. */
                int type_ok = is_bit_compound_op(assign_op)
                    ? type_is_integer(&lt) : type_is_numeric(&lt);
                if (!type_ok) {
                    codegen_report_error(ctx, expr->ast_token.line, expr->ast_token.col, "compound assignment not supported for this type");
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "0 /* invalid compound assignment */;\n");
                    return;
                }
                /* Primitive compound assignments fall through to the normal
                   codegen_expr path below. */
            }
        }

        if (lhs->ast_kind == AST_ARRAY_ACCESS) {
            Type at = lhs->ast_children[0]->ast_resolved_type;
            if (at.is_array) {
                emit_stmt_call_retains(ctx, expr, indent);
                emit_compound_lvalue_temp(ctx, expr, indent);
                indent_line(ctx, indent);
                codegen_expr(ctx, expr);
                fprintf(ctx->out, ";\n");
                emit_stmt_call_releases(ctx, expr, indent);
                return;
            }
        }

        if (lhs->ast_kind == AST_IDENT && lt.type_kind == TYPE_CLASS &&
            !lt.is_weak && !lt.is_unowned) {
            emit_stmt_call_retains(ctx, expr, indent);

            int id = ctx->assign_tmp_id++;
            int rhs_owned = (expr_is_owned(rhs));

            indent_line(ctx, indent);
            fprintf(ctx->out, "void* _my_assign_%d = ", id);
            if (!rhs_owned) {
                fprintf(ctx->out, "mylang_retain(");
                codegen_expr(ctx, rhs);
                fprintf(ctx->out, ")");
            } else {
                codegen_expr(ctx, rhs);
            }
            fprintf(ctx->out, ";\n");

            indent_line(ctx, indent);
            fprintf(ctx->out, "mylang_release(");
            codegen_expr(ctx, lhs);
            fprintf(ctx->out, ");\n");

            indent_line(ctx, indent);
            codegen_expr(ctx, lhs);
            fprintf(ctx->out, " = _my_assign_%d;\n", id);

            emit_stmt_call_releases(ctx, expr, indent);
            return;
        }

        if (lhs->ast_kind == AST_IDENT && lt.is_weak && lt.type_kind == TYPE_INTERFACE) {
            emit_stmt_call_retains(ctx, expr, indent);
            resolve_type(rhs);
            Type rt = rhs->ast_resolved_type;
            int rhs_owned = (expr_is_owned(rhs));

            /* release old weak ref first */
            indent_line(ctx, indent);
            fprintf(ctx->out, "mylang_weak_release(%s.wr);\n", lhs->ast_token.text);

            if (rt.is_weak && rt.type_kind == TYPE_INTERFACE) {
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.wr = mylang_weak_copy(", lhs->ast_token.text);
                codegen_expr(ctx, rhs);
                fprintf(ctx->out, ".wr);\n");
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.vt = ", lhs->ast_token.text);
                codegen_expr(ctx, rhs);
                fprintf(ctx->out, ".vt;\n");
            } else if (rt.type_kind == TYPE_INTERFACE) {
                if (rhs_owned) {
                    int id = ctx->assign_tmp_id++;
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s _wassign%d = ", lt.class_name, id);
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ";\n");
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.wr = mylang_weak_init(_wassign%d.data);\n",
                            lhs->ast_token.text, id);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.vt = _wassign%d.vtable;\n",
                            lhs->ast_token.text, id);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "mylang_release(_wassign%d.data);\n", id);
                } else {
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.wr = mylang_weak_init(", lhs->ast_token.text);
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ".data);\n");
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.vt = ", lhs->ast_token.text);
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ".vtable;\n");
                }
            } else if (rt.type_kind == TYPE_CLASS) {
                if (rhs_owned) {
                    int id = ctx->assign_tmp_id++;
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "void* _wassign%d = ", id);
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ";\n");
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.wr = mylang_weak_init(_wassign%d);\n",
                            lhs->ast_token.text, id);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.vt = &%s_%s_vtable;\n",
                            lhs->ast_token.text, rt.class_name, lt.class_name);
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "mylang_release(_wassign%d);\n", id);
                } else {
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.wr = mylang_weak_init(", lhs->ast_token.text);
                    codegen_expr(ctx, rhs);
                    fprintf(ctx->out, ");\n");
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "%s.vt = &%s_%s_vtable;\n",
                            lhs->ast_token.text, rt.class_name, lt.class_name);
                }
            } else if (type_is_null(&rt)) {
                /* null -> weak interface variable (old .wr released above) */
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.wr = NULL;\n", lhs->ast_token.text);
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.vt = NULL;\n", lhs->ast_token.text);
            } else {
                codegen_report_error(ctx, rhs->ast_token.line, rhs->ast_token.col, "cannot assign to weak interface '%s' from this type", lt.class_name);
            }

            emit_stmt_call_releases(ctx, expr, indent);
            return;
        }

        if (lhs->ast_kind == AST_IDENT && lt.type_kind == TYPE_INTERFACE) {
            emit_stmt_call_retains(ctx, expr, indent);
            resolve_type(rhs);
            Type rt = rhs->ast_resolved_type;

            int id = ctx->assign_tmp_id++;
            int rhs_owned = (expr_is_owned(rhs));

            if (rt.type_kind == TYPE_CLASS) {
                indent_line(ctx, indent);
                fprintf(ctx->out, "void* _iassign_%d = ", id);
                if (!rhs_owned) fprintf(ctx->out, "mylang_retain(");
                codegen_expr(ctx, rhs);
                if (!rhs_owned) fprintf(ctx->out, ")");
                fprintf(ctx->out, ";\n");

                indent_line(ctx, indent);
                fprintf(ctx->out, "mylang_release(%s.data);\n", lhs->ast_token.text);
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.data = _iassign_%d;\n", lhs->ast_token.text, id);
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.vtable = &%s_%s_vtable;\n",
                        lhs->ast_token.text, rt.class_name, lt.class_name);
            } else if (rt.type_kind == TYPE_INTERFACE) {
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s _iassign_%d = ", c_base_name(&lt), id);
                codegen_expr(ctx, rhs);
                fprintf(ctx->out, ";\n");

                if (!rhs_owned) {
                    indent_line(ctx, indent);
                    fprintf(ctx->out, "mylang_retain(_iassign_%d.data);\n", id);
                }

                indent_line(ctx, indent);
                fprintf(ctx->out, "mylang_release(%s.data);\n", lhs->ast_token.text);
                indent_line(ctx, indent);
                codegen_expr(ctx, lhs);
                fprintf(ctx->out, " = _iassign_%d;\n", id);
            } else if (type_is_null(&rt)) {
                /* null -> interface variable */
                indent_line(ctx, indent);
                fprintf(ctx->out, "mylang_release(%s.data);\n", lhs->ast_token.text);
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.data = NULL;\n", lhs->ast_token.text);
                indent_line(ctx, indent);
                fprintf(ctx->out, "%s.vtable = NULL;\n", lhs->ast_token.text);
            } else {
                codegen_report_error(ctx, rhs->ast_token.line, rhs->ast_token.col, "cannot assign non-class value to interface '%s'", lt.class_name);
            }

            emit_stmt_call_releases(ctx, expr, indent);
            return;
        }
    }

    emit_stmt_call_retains(ctx, expr, indent);

    if (expr->ast_kind == AST_ASSIGN || expr->ast_kind == AST_INC_DEC) {
        emit_compound_lvalue_temp(ctx, expr, indent);
    }

    indent_line(ctx, indent);
    if (expr->ast_kind == AST_CALL && expr->ast_resolved_type.type_kind == TYPE_CLASS) {
        /* discarded class return: release the +1 from callee */
        fprintf(ctx->out, "(void)mylang_release(");
        codegen_expr(ctx, expr);
        fprintf(ctx->out, ")");
    } else if (expr->ast_kind == AST_CALL && expr->ast_resolved_type.type_kind == TYPE_INTERFACE) {
        /* discarded interface return: save to temp, release .data */
        int dtid = ctx->assign_tmp_id++;
        fprintf(ctx->out, "%s _dt%d = ", c_base_name(&expr->ast_resolved_type), dtid);
        codegen_expr(ctx, expr);
        fprintf(ctx->out, "; (void)mylang_release(_dt%d.data)", dtid);
    } else if (expr->ast_kind == AST_CALL && struct_has_ref_fields(&expr->ast_resolved_type)) {
        /* discarded struct return owning reference fields: release its shares */
        int dtid = ctx->assign_tmp_id++;
        fprintf(ctx->out, "%s _dt%d = ", c_base_name(&expr->ast_resolved_type), dtid);
        codegen_expr(ctx, expr);
        fprintf(ctx->out, "; _mylang_release_%s(&_dt%d)", expr->ast_resolved_type.class_name, dtid);
    } else {
        codegen_expr(ctx, expr);
    }
    fprintf(ctx->out, ";\n");

    emit_stmt_call_releases(ctx, expr, indent);
}

static void codegen_stmt(CodegenContext* ctx, AstNode* node, int indent) {
    if (!node) return;

    switch (node->ast_kind) {
        case AST_BLOCK:
            codegen_body(ctx, node, indent);
            break;
        case AST_VAR_DECL:
            codegen_var_decl(ctx, node, indent);
            break;
        case AST_IF_STMT:
            codegen_if_stmt(ctx, node, indent);
            break;
        case AST_WHILE_STMT:
            codegen_while_stmt(ctx, node, indent);
            break;
        case AST_FOR_STMT:
            codegen_for_stmt(ctx, node, indent);
            break;
        case AST_FOREACH_STMT:
            codegen_foreach_stmt(ctx, node, indent);
            break;
        case AST_MATCH:
            codegen_match_stmt(ctx, node, indent);
            break;
        case AST_RETURN_STMT:
            codegen_return_stmt(ctx, node, indent);
            break;
        case AST_BREAK:
            codegen_break_stmt(ctx, node, indent);
            break;
        case AST_CONTINUE:
            codegen_continue_stmt(ctx, node, indent);
            break;
        case AST_EXPR_STMT:
            codegen_expr_stmt(ctx, node, indent);
            break;
        default:
            indent_line(ctx, indent);
            fprintf(ctx->out, "/* unknown stmt kind=%d */\n", node->ast_kind);
            break;
    }
}


static void codegen_method_decl(CodegenContext* ctx, AstNode* node, const char* class_name);

static void emit_interface_header_typedefs(CodegenContext* ctx) {
    extern InterfaceInfo* interface_list;
    InterfaceInfo* ii = interface_list;
    while (ii) {
        FILE* h = ctx->header;
        /* VTable definition. */
        fprintf(h, "struct %sVTable {\n", ii->name);
        fprintf(h, "    uint32_t concrete_type_id;\n");
        int j;
        for (j = 0; j < ii->method_count; j++) {
            InterfaceMethodInfo* im = &ii->methods[j];
            char rbuf[128];
            c_type_str(&im->return_type, rbuf, sizeof(rbuf));
            fprintf(h, "    %s (*%s)(void* thiz", rbuf, im->name);
            int k;
            for (k = 0; k < im->param_count; k++) {
                char pbuf[128];
                c_type_str(&im->param_types[k], pbuf, sizeof(pbuf));
                if (im->param_types[k].is_ref) {
                    fprintf(h, ", %s*", pbuf);
                } else {
                    fprintf(h, ", %s", pbuf);
                }
            }
            fprintf(h, ");\n");
        }
        fprintf(h, "};\n\n");

        ii = ii->next;
    }
}

static void emit_interface_c_helpers(CodegenContext* ctx) {
    extern InterfaceInfo* interface_list;
    InterfaceInfo* ii = interface_list;
    while (ii) {
        /* Weak interface lock helper returns a strong fat pointer */
        fprintf(ctx->out, "static %s mylang_lock_%s(WeakRef* wr, const %sVTable* vt) {\n",
                ii->name, ii->name, ii->name);
        fprintf(ctx->out, "    void* _p = mylang_lock(wr);\n");
        fprintf(ctx->out, "    %s _r;\n", ii->name);
        fprintf(ctx->out, "    _r.data = _p;\n");
        fprintf(ctx->out, "    _r.vtable = vt;\n");
        fprintf(ctx->out, "    return _r;\n");
        fprintf(ctx->out, "}\n\n");

        /* Weak interface conversion helpers */
        fprintf(ctx->out, "static Weak%s mylang_weakify_%s(%s s) {\n",
                ii->name, ii->name, ii->name);
        fprintf(ctx->out, "    Weak%s w;\n", ii->name);
        fprintf(ctx->out, "    w.wr = mylang_weak_init(s.data);\n");
        fprintf(ctx->out, "    w.vt = s.vtable;\n");
        fprintf(ctx->out, "    return w;\n");
        fprintf(ctx->out, "}\n\n");

        fprintf(ctx->out, "static Weak%s mylang_weakify_%s_owned(%s s) {\n",
                ii->name, ii->name, ii->name);
        fprintf(ctx->out, "    Weak%s w;\n", ii->name);
        fprintf(ctx->out, "    w.wr = mylang_weak_init(s.data);\n");
        fprintf(ctx->out, "    w.vt = s.vtable;\n");
        fprintf(ctx->out, "    mylang_release(s.data);\n");
        fprintf(ctx->out, "    return w;\n");
        fprintf(ctx->out, "}\n\n");

        fprintf(ctx->out, "static Weak%s mylang_weakify_%s_from_ptr(void* p, const %sVTable* vt) {\n",
                ii->name, ii->name, ii->name);
        fprintf(ctx->out, "    Weak%s w;\n", ii->name);
        fprintf(ctx->out, "    w.wr = mylang_weak_init(p);\n");
        fprintf(ctx->out, "    w.vt = vt;\n");
        fprintf(ctx->out, "    return w;\n");
        fprintf(ctx->out, "}\n\n");

        fprintf(ctx->out, "static Weak%s mylang_weakify_%s_from_ptr_owned(void* p, const %sVTable* vt) {\n",
                ii->name, ii->name, ii->name);
        fprintf(ctx->out, "    Weak%s w;\n", ii->name);
        fprintf(ctx->out, "    w.wr = mylang_weak_init(p);\n");
        fprintf(ctx->out, "    w.vt = vt;\n");
        fprintf(ctx->out, "    mylang_release(p);\n");
        fprintf(ctx->out, "    return w;\n");
        fprintf(ctx->out, "}\n\n");
        ii = ii->next;
    }
}

static void emit_interface_default_methods(CodegenContext* ctx) {
    extern InterfaceInfo* interface_list;
    InterfaceInfo* ii = interface_list;
    while (ii) {
        int j;
        for (j = 0; j < ii->method_count; j++) {
            InterfaceMethodInfo* im = &ii->methods[j];
            if (!im->interface_method_default_body) continue;

            char rbuf[128];
            c_type_str(&im->return_type, rbuf, sizeof(rbuf));
            fprintf(ctx->out, "static %s mylang_IDefault_%s_%s(void* thiz",
                    rbuf, ii->name, im->name);
            int k;
            for (k = 0; k < im->param_count; k++) {
                char pbuf[128];
                c_type_str(&im->param_types[k], pbuf, sizeof(pbuf));
                if (im->param_types[k].is_ref) {
                    fprintf(ctx->out, ", %s* %s", pbuf, im->param_names[k]);
                } else {
                    fprintf(ctx->out, ", %s %s", pbuf, im->param_names[k]);
                }
            }
            fprintf(ctx->out, ")\n");

            cleanup_push_scope(ctx);
            fprintf(ctx->out, "{\n");

            symtab_enter_scope();
            Type prev_ret = ctx->return_type;
            ctx->return_type = im->return_type;
            ClassInfo* prev_class = ctx->current_class;
            ctx->current_class = NULL;   /* default bodies have no class context */

            /* register parameters in scope */
            for (k = 0; k < im->param_count; k++) {
                symtab_insert(im->param_names[k], im->param_types[k]);
                if (im->param_types[k].is_weak && im->param_types[k].type_kind == TYPE_INTERFACE) {
                    cleanup_add_weak_interface(ctx, im->param_names[k]);
                } else if (im->param_types[k].is_weak || im->param_types[k].is_unowned) {
                    cleanup_add(ctx, im->param_names[k], 1, 0);
                }
            }

            /* default interface methods do not have a 'this' */
            ctx->is_interface_default_method = 1;

            indent_line(ctx, 1);
            if (im->interface_method_default_body) {
                codegen_set_current_file(ctx, im->interface_method_default_body->ast_token.filename);
            }
            fprintf(ctx->out, "MY_PUSH(\"%s.%s\", \"%s\", %d);\n",
                    ii->name, im->name, ctx->current_file_escaped, im->interface_method_line);

            fprintf(ctx->out, "{\n");
            AstNode* body = im->interface_method_default_body;
            if (body && body->ast_kind == AST_BLOCK) {
                AstNode* s = body->ast_children[0];
                while (s) {
                    codegen_stmt(ctx, s, 2);
                    s = s->next;
                }
            } else if (body) {
                codegen_stmt(ctx, body, 2);
            }
            cleanup_pop_scope(ctx, 2);
            fprintf(ctx->out, "}\n");

            cleanup_pop_scope(ctx, 1);
            symtab_exit_scope();
            ctx->is_interface_default_method = 0;
            indent_line(ctx, 1);
            fprintf(ctx->out, "MY_POP();\n");
            fprintf(ctx->out, "}\n\n");
            ctx->return_type = prev_ret;
            ctx->current_class = prev_class;
        }
        ii = ii->next;
    }
}

static void codegen_class_interface_vtables(CodegenContext* ctx, ClassInfo* ci) {
    const char* class_c = class_c_name(ci);
    int i;
    for (i = 0; i < ci->impl_count; i++) {
        const char* iface_name = ci->impl_names[i];
        InterfaceInfo* ii = symtab_find_interface(iface_name);
        if (!ii) continue;

        /* emit thunk functions only for methods implemented by the class;
           methods with a default body are satisfied by the shared
           mylang_IDefault_<Iface>_<method> implementation. */
        int j;
        for (j = 0; j < ii->method_count; j++) {
            InterfaceMethodInfo* im = &ii->methods[j];
            MethodInfo* cls_m = symtab_find_method_in_class(ci, im->name);
            /* A static method has no receiver and cannot implement an
               interface method (symtab_validate_impls rejects this). */
            if (cls_m && cls_m->method_is_static) cls_m = NULL;
            if (cls_m) {
                char rbuf[128];
                c_type_str(&im->return_type, rbuf, sizeof(rbuf));

                /* static RetType Class_IFace_method(void* thiz, params...) */
                fprintf(ctx->out, "static %s %s_%s_%s(void* _p", rbuf, class_c, iface_name, im->name);
                int k;
                for (k = 0; k < im->param_count; k++) {
                    char pbuf[128];
                    c_type_str(&im->param_types[k], pbuf, sizeof(pbuf));
                    if (im->param_types[k].is_ref) {
                        fprintf(ctx->out, ", %s* _a%d", pbuf, k);
                    } else {
                        fprintf(ctx->out, ", %s _a%d", pbuf, k);
                    }
                }
                fprintf(ctx->out, ") {\n");

                /* call actual method */
                fprintf(ctx->out, "    ");
                if (im->return_type.type_kind != TYPE_VOID) {
                    fprintf(ctx->out, "return ");
                }
                fprintf(ctx->out, "%s_%s((%s*)_p", class_c, im->name, class_c);
                for (k = 0; k < im->param_count; k++) {
                    fprintf(ctx->out, ", _a%d", k);
                }
                fprintf(ctx->out, ");\n");
                fprintf(ctx->out, "}\n\n");
            }
        }

        /* emit static vtable */
        fprintf(ctx->out, "static const %sVTable %s_%s_vtable = {\n", iface_name, class_c, iface_name);
        fprintf(ctx->out, "    .concrete_type_id = %u,\n", (unsigned)ci->type_id);
        for (j = 0; j < ii->method_count; j++) {
            InterfaceMethodInfo* im = &ii->methods[j];
            MethodInfo* cls_m = symtab_find_method_in_class(ci, im->name);
            if (cls_m && cls_m->method_is_static) cls_m = NULL;
            fprintf(ctx->out, "    .%s = ", im->name);
            if (cls_m) {
                fprintf(ctx->out, "%s_%s_%s", class_c, iface_name, im->name);
            } else {
                fprintf(ctx->out, "mylang_IDefault_%s_%s", iface_name, im->name);
            }
            if (j < ii->method_count - 1) fprintf(ctx->out, ",");
            fprintf(ctx->out, "\n");
        }
        fprintf(ctx->out, "};\n\n");
    }
}

static void codegen_struct_decl(CodegenContext* ctx, AstNode* node) {
    /* Struct definitions are emitted to the generated header; the .c file
       includes that header, so nothing needs to be written here. */
    (void)ctx;
    (void)node;
}

/* Emit the per-class finalizer that releases reference-counted fields before
   the object's memory is freed by mylang_release. */
static void codegen_class_destructor(CodegenContext* ctx, ClassInfo* ci, const char* class_c) {
    (void)ctx;
    fprintf(ctx->out, "void _mylang_dtor_%s(%s* p) {\n", class_c, class_c);
    int i;
    for (i = 0; i < ci->field_count; i++) {
        Type ft = ci->field_types[i];
        const char* fname = ci->field_names[i];
        if (ft.is_array) {
            char esz[64];
            array_elem_size_expr(&ft, esz, sizeof(esz));
            fprintf(ctx->out, "    mylang_array_free(&p->%s, %s, %d);\n",
                    fname, esz, array_elem_kind(&ft));
        } else if (ft.is_weak && ft.type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "    mylang_weak_release(p->%s.wr);\n", fname);
        } else if (ft.is_weak || ft.is_unowned) {
            fprintf(ctx->out, "    mylang_weak_release(p->%s);\n", fname);
        } else if (ft.type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "    mylang_release(p->%s.data);\n", fname);
        } else if (ft.type_kind == TYPE_CLASS || ft.type_kind == TYPE_OBJECT) {
            fprintf(ctx->out, "    mylang_release(p->%s);\n", fname);
        } else if (ft.type_kind == TYPE_STRUCT) {
            /* Struct fields owning reference fields are torn down with the
               struct's compiler-generated release hook. */
            StructInfo* si = symtab_find_struct(ft.class_name);
            if (si && si->has_ref_fields) {
                fprintf(ctx->out, "    _mylang_release_%s(&p->%s);\n", si->name, fname);
            }
        }
    }
    fprintf(ctx->out, "}\n\n");
}

static void codegen_class_decl(CodegenContext* ctx, AstNode* node) {
    ClassInfo* ci = symtab_find_class_by_mangled(node->ast_token.text);
    if (!ci) ci = symtab_find_class(node->ast_token.text);
    if (!ci) return;
    if (ci->is_generic) return;

    const char* class_c = class_c_name(ci);

    /* Fields of type (ref-struct)[] are rejected: MyArray cannot run
       per-element retain/release hooks yet. */
    {
        int i;
        for (i = 0; i < ci->field_count; i++) {
            if (type_is_ref_struct_array(&ci->field_types[i])) {
                codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "arrays of struct '%s' with reference fields are not supported yet (field '%s')", ci->field_types[i].class_name, ci->field_names[i]);
            }
        }
    }

    codegen_class_destructor(ctx, ci, class_c);

    /* emit method declarations */
    AstNode* m = node->ast_children[0];
    while (m) {
        codegen_method_decl(ctx, m, class_c);
        m = m->next;
    }

    /* emit interface thunks and vtables */
    codegen_class_interface_vtables(ctx, ci);
}

/* Per-struct retain/release hooks for structs that own reference fields
   (class/interface/object strong fields, weak/unowned shares, or nested
   structs with any of those).  Copies call the retain hook; destruction
   (scope exit, overwrite, field teardown) calls the release hook. */
static void codegen_struct_hooks(CodegenContext* ctx, StructInfo* si) {
    if (!si->has_ref_fields) return;

    int i;
    fprintf(ctx->out, "void _mylang_retain_%s(%s* p) {\n", si->name, si->name);
    for (i = 0; i < si->field_count; i++) {
        Type* ft = &si->field_types[i];
        const char* fn = si->field_names[i];
        if (ft->is_weak && ft->type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "    p->%s.wr = mylang_weak_copy(p->%s.wr);\n", fn, fn);
        } else if (ft->is_weak || ft->is_unowned) {
            fprintf(ctx->out, "    p->%s = mylang_weak_copy(p->%s);\n", fn, fn);
        } else if (ft->type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "    mylang_retain(p->%s.data);\n", fn);
        } else if (ft->type_kind == TYPE_CLASS || ft->type_kind == TYPE_OBJECT) {
            fprintf(ctx->out, "    mylang_retain(p->%s);\n", fn);
        } else if (ft->type_kind == TYPE_STRUCT) {
            StructInfo* dep = symtab_find_struct(ft->class_name);
            if (dep && dep->has_ref_fields) {
                fprintf(ctx->out, "    _mylang_retain_%s(&p->%s);\n", dep->name, fn);
            }
        }
    }
    fprintf(ctx->out, "}\n\n");

    fprintf(ctx->out, "void _mylang_release_%s(%s* p) {\n", si->name, si->name);
    for (i = 0; i < si->field_count; i++) {
        Type* ft = &si->field_types[i];
        const char* fn = si->field_names[i];
        if (ft->is_weak && ft->type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "    mylang_weak_release(p->%s.wr);\n", fn);
        } else if (ft->is_weak || ft->is_unowned) {
            fprintf(ctx->out, "    mylang_weak_release(p->%s);\n", fn);
        } else if (ft->type_kind == TYPE_INTERFACE) {
            fprintf(ctx->out, "    mylang_release(p->%s.data);\n", fn);
        } else if (ft->type_kind == TYPE_CLASS || ft->type_kind == TYPE_OBJECT) {
            fprintf(ctx->out, "    mylang_release(p->%s);\n", fn);
        } else if (ft->type_kind == TYPE_STRUCT) {
            StructInfo* dep = symtab_find_struct(ft->class_name);
            if (dep && dep->has_ref_fields) {
                fprintf(ctx->out, "    _mylang_release_%s(&p->%s);\n", dep->name, fn);
            }
        }
    }
    fprintf(ctx->out, "}\n\n");
}

static void codegen_struct_method_decl(CodegenContext* ctx, AstNode* node, const char* struct_name) {
    if (node->ast_resolved_type.is_array) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "method '%s.%s' cannot return array by value", struct_name, node->ast_token.text);
    }
    char ret_buf[128];
    c_type_str(&node->ast_resolved_type, ret_buf, sizeof(ret_buf));
    fprintf(ctx->out, "%s %s_%s(%s* thiz", ret_buf, struct_name, node->ast_token.text, struct_name);
    AstNode* params = NULL; AstNode* body = NULL;
    if (node->ast_child_count == 2) { params = node->ast_children[0]; body = node->ast_children[1]; }
    else { body = node->ast_children[0]; }
    { AstNode* p = params; while (p) { fprintf(ctx->out, ", ");
        if (p->ast_resolved_type.is_array && !type_is_ref(&p->ast_resolved_type)) {
            codegen_report_error(ctx, p->ast_token.line, p->ast_token.col, "array parameter '%s' must be ref", p->ast_token.text);
        }
        if (type_is_ref_struct_array(&p->ast_resolved_type)) {
            codegen_report_error(ctx, p->ast_token.line, p->ast_token.col, "arrays of struct '%s' with reference fields are not supported yet", p->ast_resolved_type.class_name);
        }
        char pt[128]; c_type_str(&p->ast_resolved_type, pt, sizeof(pt));
        if (type_is_ref(&p->ast_resolved_type)) {
            fprintf(ctx->out, "%s* %s", pt, p->ast_token.text);
        } else {
            fprintf(ctx->out, "%s %s", pt, p->ast_token.text);
        }
        p = p->next; } }
    fprintf(ctx->out, ")\n{\n");
    cleanup_push_scope(ctx); symtab_enter_scope();
    Type prev_ret = ctx->return_type;
    ctx->return_type = node->ast_resolved_type;
    /* 'this' is a borrowed pointer to the receiver: no retain/release. */
    Type thiz_type; memset(&thiz_type, 0, sizeof(thiz_type));
    thiz_type.type_kind = TYPE_STRUCT;
    CHECK_STRSCPY(strscpy(thiz_type.class_name, struct_name, sizeof(thiz_type.class_name)), "struct name too long");
    thiz_type.is_ref = 1; symtab_insert("this", thiz_type);
    { AstNode* p = params; while (p) { symtab_insert(p->ast_token.text, p->ast_resolved_type);
        if (p->ast_resolved_type.is_weak && p->ast_resolved_type.type_kind == TYPE_INTERFACE) {
            cleanup_add_weak_interface(ctx, p->ast_token.text);
        } else if (p->ast_resolved_type.is_weak || p->ast_resolved_type.is_unowned) {
            cleanup_add(ctx, p->ast_token.text, 1, 0);
        }
        p = p->next; } }

    indent_line(ctx, 1);
    codegen_set_current_file(ctx, node->ast_token.filename);
    fprintf(ctx->out, "MY_PUSH(\"%s.%s\", \"%s\", %d);\n", struct_name, node->ast_token.text, ctx->current_file_escaped, node->ast_token.line);

    /* By-value struct parameters owning reference fields: the callee's copy
       retains on entry and is released at scope exit. */
    { AstNode* p = params; while (p) {
        if (struct_has_ref_fields(&p->ast_resolved_type) && !type_is_ref(&p->ast_resolved_type)) {
            indent_line(ctx, 1);
            fprintf(ctx->out, "_mylang_retain_%s(&%s);\n", p->ast_resolved_type.class_name, p->ast_token.text);
            cleanup_add_struct_dtor(ctx, p->ast_token.text, p->ast_resolved_type.class_name);
        }
        p = p->next; } }

    fprintf(ctx->out, "{\n");
    if (body && body->ast_kind == AST_BLOCK) { AstNode* s = body->ast_children[0];
        while (s) { codegen_stmt(ctx, s, 2); s = s->next; } }
    cleanup_pop_scope(ctx, 2);
    fprintf(ctx->out, "}\n");

    cleanup_pop_scope(ctx, 1);
    symtab_exit_scope();
    indent_line(ctx, 1);
    fprintf(ctx->out, "MY_POP();\n");
    fprintf(ctx->out, "}\n\n");
    ctx->return_type = prev_ret;
}

static void codegen_struct_methods(CodegenContext* ctx, AstNode* node) {
    StructInfo* si = symtab_find_struct(node->ast_token.text);
    if (!si) return;
    codegen_struct_hooks(ctx, si);
    AstNode* m = node->ast_child_count > 0 ? node->ast_children[0] : NULL;
    while (m) {
        codegen_struct_method_decl(ctx, m, si->name);
        m = m->next;
    }
}


static void codegen_method_decl(CodegenContext* ctx, AstNode* node, const char* class_name) {
    if (node->ast_resolved_type.is_array) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "method '%s.%s' cannot return array by value", class_name, node->ast_token.text);
    }
    if (node->ast_is_native) {
        /* Native methods are declared in the generated header and implemented
           by the user in a separate .c file. */
        return;
    }
    char ret_buf[128];
    c_type_str(&node->ast_resolved_type, ret_buf, sizeof(ret_buf));
    int is_static = node->ast_is_static;
    if (is_static) {
        /* Static methods have no receiver parameter. */
        fprintf(ctx->out, "%s %s_%s(", ret_buf, class_name, node->ast_token.text);
    } else {
        fprintf(ctx->out, "%s %s_%s(%s* thiz", ret_buf, class_name, node->ast_token.text, class_name);
    }
    AstNode* params = NULL; AstNode* body = NULL;
    if (node->ast_child_count == 2) { params = node->ast_children[0]; body = node->ast_children[1]; }
    else { body = node->ast_children[0]; }
    if (is_static && !params) fprintf(ctx->out, "void");
    { AstNode* p = params; int pfirst = is_static; while (p) { if (!pfirst) fprintf(ctx->out, ", "); pfirst = 0;
        if (p->ast_resolved_type.is_array && !type_is_ref(&p->ast_resolved_type)) {
            codegen_report_error(ctx, p->ast_token.line, p->ast_token.col, "array parameter '%s' must be ref", p->ast_token.text);
        }
        if (type_is_ref_struct_array(&p->ast_resolved_type)) {
            codegen_report_error(ctx, p->ast_token.line, p->ast_token.col, "arrays of struct '%s' with reference fields are not supported yet", p->ast_resolved_type.class_name);
        }
        char pt[128]; c_type_str(&p->ast_resolved_type, pt, sizeof(pt));
        if (type_is_ref(&p->ast_resolved_type)) {
            fprintf(ctx->out, "%s* %s", pt, p->ast_token.text);
        } else {
            fprintf(ctx->out, "%s %s", pt, p->ast_token.text);
        }
        p = p->next; } }
    fprintf(ctx->out, ")\n{\n");
    cleanup_push_scope(ctx); symtab_enter_scope();
    Type prev_ret = ctx->return_type;
    ctx->return_type = node->ast_resolved_type;
    ClassInfo* prev_class = ctx->current_class;
    ctx->current_class = symtab_find_class(class_name);
    int prev_static = ctx->current_method_is_static;
    ctx->current_method_is_static = is_static;
    if (!is_static) {
        Type thiz_type; memset(&thiz_type, 0, sizeof(thiz_type));
        thiz_type.type_kind = TYPE_CLASS;
        CHECK_STRSCPY(strscpy(thiz_type.class_name, class_name, sizeof(thiz_type.class_name)), "class name too long");
        thiz_type.is_pointer = 1; symtab_insert("this", thiz_type);
    }
    { AstNode* p = params; while (p) { symtab_insert(p->ast_token.text, p->ast_resolved_type);
        if (p->ast_resolved_type.is_weak && p->ast_resolved_type.type_kind == TYPE_INTERFACE) {
            cleanup_add_weak_interface(ctx, p->ast_token.text);
        } else if (p->ast_resolved_type.is_weak || p->ast_resolved_type.is_unowned) {
            cleanup_add(ctx, p->ast_token.text, 1, 0);
        }
        p = p->next; } }

    indent_line(ctx, 1);
    codegen_set_current_file(ctx, node->ast_token.filename);
    fprintf(ctx->out, "MY_PUSH(\"%s.%s\", \"%s\", %d);\n", class_name, node->ast_token.text, ctx->current_file_escaped, node->ast_token.line);

    /* By-value struct parameters owning reference fields: the callee's copy
       retains on entry and is released at scope exit. */
    { AstNode* p = params; while (p) {
        if (struct_has_ref_fields(&p->ast_resolved_type) && !type_is_ref(&p->ast_resolved_type)) {
            indent_line(ctx, 1);
            fprintf(ctx->out, "_mylang_retain_%s(&%s);\n", p->ast_resolved_type.class_name, p->ast_token.text);
            cleanup_add_struct_dtor(ctx, p->ast_token.text, p->ast_resolved_type.class_name);
        }
        p = p->next; } }

    fprintf(ctx->out, "{\n");
    if (body && body->ast_kind == AST_BLOCK) { AstNode* s = body->ast_children[0];
        while (s) { codegen_stmt(ctx, s, 2); s = s->next; } }
    cleanup_pop_scope(ctx, 2);
    fprintf(ctx->out, "}\n");

    cleanup_pop_scope(ctx, 1);
    symtab_exit_scope();
    indent_line(ctx, 1);
    fprintf(ctx->out, "MY_POP();\n");
    fprintf(ctx->out, "}\n\n");
    ctx->return_type = prev_ret;
    ctx->current_class = prev_class;
    ctx->current_method_is_static = prev_static;
}
static void codegen_func_decl(CodegenContext* ctx, AstNode* node) {
    const char* func_name = node->ast_token.text;
    if (strcmp(func_name, "main") == 0) {
        func_name = "_my_main";
        ctx->has_main = 1;
        ctx->main_return_type = node->ast_resolved_type;
    }

    /* return type */
    if (node->ast_resolved_type.is_array) {
        codegen_report_error(ctx, node->ast_token.line, node->ast_token.col, "function '%s' cannot return array by value", func_name);
    }
    {
        char ret_buf[128];
        c_type_str(&node->ast_resolved_type, ret_buf, sizeof(ret_buf));
        fprintf(ctx->out, "%s %s(", ret_buf, func_name);
    }

    /* parameters */
    AstNode* params = NULL;
    AstNode* body   = NULL;

    if (node->ast_child_count == 2) {
        params = node->ast_children[0];
        body   = node->ast_children[1];
    } else {
        body = node->ast_children[0];
    }

    {
        AstNode* p = params;
        int first = 1;
        while (p) {
            if (!first) fprintf(ctx->out, ", ");
            if (p->ast_resolved_type.is_array && !type_is_ref(&p->ast_resolved_type)) {
                codegen_report_error(ctx, p->ast_token.line, p->ast_token.col, "array parameter '%s' must be ref", p->ast_token.text);
            }
            if (type_is_ref_struct_array(&p->ast_resolved_type)) {
                codegen_report_error(ctx, p->ast_token.line, p->ast_token.col, "arrays of struct '%s' with reference fields are not supported yet", p->ast_resolved_type.class_name);
            }
            char ptype_buf[128];
            c_type_str(&p->ast_resolved_type, ptype_buf, sizeof(ptype_buf));
            if (type_is_ref(&p->ast_resolved_type)) {
                fprintf(ctx->out, "%s* %s", ptype_buf, p->ast_token.text);
            } else {
                fprintf(ctx->out, "%s %s", ptype_buf, p->ast_token.text);
            }
            first = 0;
            p = p->next;
        }
    }
    fprintf(ctx->out, ")\n");

    /* body */
    cleanup_push_scope(ctx);
    fprintf(ctx->out, "{\n");

    symtab_enter_scope();

    Type prev_ret = ctx->return_type;
    ctx->return_type = node->ast_resolved_type;
    ClassInfo* prev_class = ctx->current_class;
    ctx->current_class = NULL;

    /* register parameters in scope */
    {
        AstNode* p = params;
        while (p) {
            symtab_insert(p->ast_token.text, p->ast_resolved_type);
            if (p->ast_resolved_type.is_weak && p->ast_resolved_type.type_kind == TYPE_INTERFACE) {
                cleanup_add_weak_interface(ctx, p->ast_token.text);
            } else if (p->ast_resolved_type.is_weak || p->ast_resolved_type.is_unowned) {
                cleanup_add(ctx, p->ast_token.text, 1, 0);
            }
            p = p->next;
        }
    }

    indent_line(ctx, 1);
    codegen_set_current_file(ctx, node->ast_token.filename);
    fprintf(ctx->out, "MY_PUSH(\"%s\", \"%s\", %d);\n", func_name, ctx->current_file_escaped, node->ast_token.line);

    /* By-value struct parameters owning reference fields: the callee's copy
       retains on entry and is released at scope exit. */
    {
        AstNode* p = params;
        while (p) {
            if (struct_has_ref_fields(&p->ast_resolved_type) && !type_is_ref(&p->ast_resolved_type)) {
                indent_line(ctx, 1);
                fprintf(ctx->out, "_mylang_retain_%s(&%s);\n", p->ast_resolved_type.class_name, p->ast_token.text);
                cleanup_add_struct_dtor(ctx, p->ast_token.text, p->ast_resolved_type.class_name);
            }
            p = p->next;
        }
    }

    fprintf(ctx->out, "{\n");

    /* walk body statements */
    if (body && body->ast_kind == AST_BLOCK) {
        AstNode* s = body->ast_children[0];
        while (s) {
            codegen_stmt(ctx, s, 2);
            s = s->next;
        }
    }

    cleanup_pop_scope(ctx, 2);
    fprintf(ctx->out, "}\n");

    cleanup_pop_scope(ctx, 1);
    symtab_exit_scope();
    indent_line(ctx, 1);
    fprintf(ctx->out, "MY_POP();\n");
    fprintf(ctx->out, "}\n\n");
    ctx->return_type = prev_ret;
    ctx->current_class = prev_class;
}

/* Top-level and static-class-member const declarations.  Scalar consts
   become a C `static const` initialized with the literal; string consts
   become a global String* initialized in main (see
   emit_const_string_inits).  Emitted after the class forward declarations,
   so the String* form only needs the forward-declared String. */
static void emit_const_decls(CodegenContext* ctx) {
    FILE* h = ctx->header;
    ConstInfo* ci = symtab_first_const();
    while (ci) {
        char cname[2 * NAME_BUF_SIZE];
        const_c_name(ci, cname, sizeof(cname));
        if (ci->const_is_string) {
            fprintf(h, "static String* %s;\n", cname);
        } else {
            char buf[128];
            c_type_str(&ci->const_type, buf, sizeof(buf));
            fprintf(h, "static const %s %s = %s;\n", buf, cname, ci->const_literal);
        }
        ci = ci->next;
    }
}

/* Registers top-level string const literals with the string-encryption
   table.  They live in the symbol table, outside the program AST, so
   codegen_collect_xor_strings never visits them. */
static void codegen_collect_xor_consts(CodegenContext* ctx) {
    ConstInfo* ci = symtab_first_const();
    while (ci) {
        if (ci->const_is_string && strlen(ci->const_literal) > 0) {
            ci->xor_str_id = ++ctx->xor_str_id;
            emit_xor_string_array_decl(ctx, ci->const_literal, ci->xor_str_id);
        }
        ci = ci->next;
    }
}

/* Initializes top-level and static-class-member string consts; emitted at
   the start of the real C main, before _my_main(). */
static void emit_const_string_inits(CodegenContext* ctx) {
    ConstInfo* ci = symtab_first_const();
    while (ci) {
        if (ci->const_is_string) {
            char cname[2 * NAME_BUF_SIZE];
            const_c_name(ci, cname, sizeof(cname));
            if (ctx->xor_strings) {
                size_t len = strlen(ci->const_literal);
                if (len == 0) {
                    fprintf(ctx->out, "    %s = mylang_string_new_encrypted(MYLANG_TID_String, NULL, 0, 1);\n",
                            cname);
                } else {
                    int id = ci->xor_str_id;
                    fprintf(ctx->out, "    %s = mylang_string_new_encrypted(MYLANG_TID_String, _xs%d, %zu, %u);\n",
                            cname, id, len, (unsigned)xor_string_key(id));
                }
            } else {
                fprintf(ctx->out, "    %s = mylang_string_new(MYLANG_TID_String, \"", cname);
                emit_c_string_literal(ctx, ci->const_literal);
                fprintf(ctx->out, "\");\n");
            }
        }
        ci = ci->next;
    }
}

/* Releases top-level and static-class-member string consts; emitted after
   _my_main() returns so the MyLang leak checker and the CRT debug heap see a
   balanced alloc/release. */
static void emit_const_string_releases(CodegenContext* ctx) {
    ConstInfo* ci = symtab_first_const();
    while (ci) {
        if (ci->const_is_string) {
            char cname[2 * NAME_BUF_SIZE];
            const_c_name(ci, cname, sizeof(cname));
            fprintf(ctx->out, "    mylang_release(%s);\n", cname);
            fprintf(ctx->out, "    %s = NULL;\n", cname);
        }
        ci = ci->next;
    }
}

void codegen_program(AstNode* program, FILE* out, FILE* header,
                     const char* source_file, int leak_check,
                     const char* header_include_name, int xor_strings) {
    CodegenContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ctx.header = header;
    ctx.header_include_name = header_include_name ? header_include_name : "";
    ctx.last_loc_line = -1;
    s_last_codegen_error = 0;
    ctx.xor_strings = xor_strings;
    ctx.xor_str_id = 0;
    ctx.source_file = source_file ? source_file : "";
    escape_source_file(&ctx, ctx.source_file);
    codegen_set_current_file(&ctx, ctx.source_file);

    if (symtab_validate_impls() != 0 || symtab_validate_structs() != 0) {
        fprintf(stderr, "error: semantic errors found, no output generated\n");
        ctx.codegen_error = 1;
        s_last_codegen_error = 1;
        return;
    }

    preinstantiate_generic_types(program);

    /* ---- Emit header ---- */
    emit_header_preamble(&ctx);
    emit_header_forward_decls(&ctx);
    /* Enum typedefs come before struct/class definitions so that enum-typed
       fields can name the completed type. */
    emit_enum_typedefs(&ctx);
    /* Top-level consts: scalars as static const, strings as globals that
       main initializes. */
    emit_const_decls(&ctx);

    /* Emit struct and class definitions before interface typedefs so that
       interface method signatures can reference value types like SdlEvent.
       Structs are emitted in dependency order (inner before outer), and
       struct types embedded in class fields are emitted before the class. */
    {
        AstNode* decl = program->ast_children[0];
        while (decl) {
            if (decl->ast_kind == AST_STRUCT_DECL) {
                StructInfo* si = symtab_find_struct(decl->ast_token.text);
                if (si) emit_struct_def_ordered(&ctx, si);
            } else if (decl->ast_kind == AST_CLASS_DECL) {
                ClassInfo* ci = symtab_find_class_by_mangled(decl->ast_token.text);
                if (!ci) ci = symtab_find_class(decl->ast_token.text);
                if (ci && !ci->is_generic) {
                    emit_class_field_struct_deps(&ctx, ci);
                    emit_class_struct_def_to_header(&ctx, ci, class_c_name(ci));
                }
            }
            decl = decl->next;
        }
        /* emit concrete generic class instantiations */
        ClassInfo* ci = class_list;
        while (ci) {
            if (ci->is_instantiation && ci->generic_ast) {
                emit_class_field_struct_deps(&ctx, ci);
                emit_class_struct_def_to_header(&ctx, ci, class_c_name(ci));
            }
            ci = ci->next;
        }
    }

    emit_interface_header_typedefs(&ctx);
    emit_header_type_ids(&ctx);
    emit_header_destructor_prototypes(&ctx);
    emit_header_function_prototypes(&ctx);
    emit_header_method_prototypes(&ctx);
    emit_header_postamble(&ctx);

    /* ---- Emit .c ---- */
    fprintf(ctx.out, "/* Generated by MyLang compiler */\n");
    if (leak_check) {
        fprintf(ctx.out, "#define MYLANG_LEAK_CHECK\n");
    }
    fprintf(ctx.out, "#define _CRTDBG_MAP_ALLOC\n");
    fprintf(ctx.out, "#include \"runtime.h\"\n");
    fprintf(ctx.out, "#include \"%s\"\n\n", ctx.header_include_name);
    fprintf(ctx.out, "#include <stdio.h>\n");
    fprintf(ctx.out, "#include <stdlib.h>\n");
    fprintf(ctx.out, "#include <stddef.h>\n");
    fprintf(ctx.out, "#include <stdint.h>\n");
    fprintf(ctx.out, "#ifdef _MSC_VER\n");
    fprintf(ctx.out, "#include <intrin.h>\n");
    fprintf(ctx.out, "#include <crtdbg.h>\n");
    fprintf(ctx.out, "/* Loops always emit a _my_continue label; it is unreferenced when the body has no 'continue'. */\n");
    fprintf(ctx.out, "#pragma warning(disable: 4102)\n");
    fprintf(ctx.out, "#else\n");
    fprintf(ctx.out, "#define __debugbreak() __builtin_trap()\n");
    fprintf(ctx.out, "#endif\n\n");

    if (ctx.xor_strings) {
        codegen_collect_xor_strings(&ctx, program);
        codegen_collect_xor_defaults(&ctx);
        codegen_collect_xor_consts(&ctx);
        fprintf(ctx.out, "\n");
    }

    emit_interface_c_helpers(&ctx);
    emit_interface_default_methods(&ctx);

    AstNode* decl = program->ast_children[0];
    while (decl) {
        if (decl->ast_kind == AST_CLASS_DECL) {
            codegen_class_decl(&ctx, decl);
        } else if (decl->ast_kind == AST_STRUCT_DECL) {
            codegen_struct_methods(&ctx, decl);
        }
        decl = decl->next;
    }

    /* emit concrete generic class instantiations */
    {
        ClassInfo* ci = class_list;
        while (ci) {
            if (ci->is_instantiation && ci->generic_ast) {
                codegen_class_decl(&ctx, ci->generic_ast);
            }
            ci = ci->next;
        }
    }

    decl = program->ast_children[0];
    while (decl) {
        if (decl->ast_kind == AST_FUNC_DECL) {
            codegen_func_decl(&ctx, decl);
        }
        decl = decl->next;
    }

    s_last_codegen_error = ctx.codegen_error;

    if (ctx.has_main) {
        fprintf(ctx.out, "int main(void) {\n");
        fprintf(ctx.out, "#ifdef _MSC_VER\n");
        fprintf(ctx.out, "    _set_abort_behavior(0, _WRITE_ABORT_MSG);\n");
        fprintf(ctx.out, "    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);\n");
        fprintf(ctx.out, "    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);\n");
        fprintf(ctx.out, "    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);\n");
        fprintf(ctx.out, "#endif\n");
        emit_const_string_inits(&ctx);
        if (ctx.main_return_type.type_kind == TYPE_VOID) {
            fprintf(ctx.out, "    _my_main();\n");
            emit_const_string_releases(&ctx);
            fprintf(ctx.out, "#ifdef _DEBUG\n");
            fprintf(ctx.out, "    _CrtDumpMemoryLeaks();\n");
            fprintf(ctx.out, "    fflush(stderr);\n");
            fprintf(ctx.out, "#endif\n");
            fprintf(ctx.out, "    return 0;\n");
        } else {
            fprintf(ctx.out, "    %s _ret = _my_main();\n", c_base_name(&ctx.main_return_type));
            emit_const_string_releases(&ctx);
            fprintf(ctx.out, "#ifdef _DEBUG\n");
            fprintf(ctx.out, "    _CrtDumpMemoryLeaks();\n");
            fprintf(ctx.out, "    fflush(stderr);\n");
            fprintf(ctx.out, "#endif\n");
            fprintf(ctx.out, "    return (int)_ret;\n");
        }
        fprintf(ctx.out, "}\n");
    }
}
