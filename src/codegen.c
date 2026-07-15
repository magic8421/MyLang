#include "codegen.h"
#include "symtab.h"
#include "util.h"
#include "runtime.h"
#include <string.h>
#include <stdio.h>

/* Forward declarations used by helper functions defined before their
   implementation. */
typedef struct CodegenContext CodegenContext;
static void codegen_expr(CodegenContext* ctx, AstNode* node, FILE* out);
static void indent_line(FILE* out, int indent);
static void emit_array_ptr_expr(CodegenContext* ctx, AstNode* arr_node, FILE* out);

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
    char        array_elem_size_expr[64];
} CleanupEntry;

struct CodegenContext {
    const char* source_file;
    char        source_file_escaped[1024];
    Type        return_type;
    int         has_main;
    Type        main_return_type;
    int         codegen_error;
    int         guard_tmp_id;
    CleanupEntry cleanup_entries[MAX_CLEANUP];
    int          cleanup_count;
    int          assign_tmp_id;
    int          subexpr_tmp_id;
    int          cleanup_scope_stack[MAX_SCOPE];
    int          cleanup_scope_depth;
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

static const char* c_base_name(const Type* t) {
    if (t->is_weak) return "WeakRef";
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

static void c_weak_interface_name(const Type* t, char* buf, size_t bufsz) {
    snprintf(buf, bufsz, "Weak%s", t->class_name);
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
    if (et.is_weak && et.type_kind == TYPE_INTERFACE) {
        char winame[128];
        c_weak_interface_name(&et, winame, sizeof(winame));
        snprintf(buf, bufsz, "%s", winame);
    } else if (et.type_kind == TYPE_CLASS && !et.is_weak) {
        /* class array stores pointers to objects */
        snprintf(buf, bufsz, "%s*", c_base_name(&et));
    } else {
        snprintf(buf, bufsz, "%s", c_base_name(&et));
    }
}

static void array_elem_size_expr(const Type* arr_type, char* buf, int bufsz) {
    Type et = array_elem_type(arr_type);
    if (et.type_kind == TYPE_CLASS && !et.is_weak) {
        snprintf(buf, bufsz, "sizeof(void*)");
    } else if (et.type_kind == TYPE_INTERFACE && !et.is_weak) {
        snprintf(buf, bufsz, "sizeof(%s)", c_base_name(&et));
    } else if (et.is_weak && et.type_kind == TYPE_INTERFACE) {
        char winame[128];
        c_weak_interface_name(&et, winame, sizeof(winame));
        snprintf(buf, bufsz, "sizeof(%s)", winame);
    } else if (et.is_weak) {
        snprintf(buf, bufsz, "sizeof(WeakRef*)");
    } else {
        snprintf(buf, bufsz, "sizeof(%s)", c_base_name(&et));
    }
}

static int array_elem_kind(const Type* arr_type) {
    Type et = array_elem_type(arr_type);
    if (et.is_weak) {
        if (et.type_kind == TYPE_INTERFACE) return MYLANG_ELEM_WEAK_IFACE;
        return MYLANG_ELEM_WEAK_CLASS;
    }
    if (et.type_kind == TYPE_INTERFACE) return MYLANG_ELEM_INTERFACE;
    if (et.type_kind == TYPE_CLASS) return MYLANG_ELEM_CLASS;
    if (et.type_kind == TYPE_STRUCT) return MYLANG_ELEM_STRUCT;
    return MYLANG_ELEM_PRIMITIVE;
}

static void emit_array_data_expr(CodegenContext* ctx, AstNode* arr_node, FILE* out) {
    /* codegen_expr already dereferences ref parameters, so the resulting
       expression is always a MyArray value. */
    fprintf(out, "(");
    codegen_expr(ctx, arr_node, out);
    fprintf(out, ")");
    fprintf(out, ".data");
}

static void emit_array_length_expr(CodegenContext* ctx, AstNode* arr_node, FILE* out) {
    fprintf(out, "(");
    codegen_expr(ctx, arr_node, out);
    fprintf(out, ")");
    fprintf(out, ".length");
}

static void preinstantiate_generic_types(AstNode* node) {
    if (!node) return;
    if (node->ast_resolved_type.type_kind == TYPE_CLASS && node->ast_resolved_type.type_arg_count > 0) {
        symtab_instantiate_class_from_type(&node->ast_resolved_type);
    }
    int i;
    for (i = 0; i < node->ast_child_count && i < 4; i++) {
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


static Type resolve_type(AstNode* node);

static Type resolve_type(AstNode* node) {
    Type t;
    memset(&t, 0, sizeof(t));

    switch (node->ast_kind) {
        case AST_INT_LIT:
            t.type_kind = TYPE_I32;
            t.type_id = TYPE_ID_I32;
            break;

        case AST_CHAR_LIT:
            t.type_kind = TYPE_I8;
            t.type_id = TYPE_ID_I8;
            break;

        case AST_IDENT: {
            SymEntry* e = symtab_lookup(node->ast_token.text);
            if (e) {
                t = e->type;
                /* A ref parameter is a pointer in C, but its value type
                   for the caller is the referenced object. */
                t.is_ref = 0;
            }
            break;
        }

        case AST_BINARY: {
            TokenKind op = node->ast_token.kind;
            if (op == TOK_EQ || op == TOK_NE ||
                op == TOK_LT || op == TOK_LE ||
                op == TOK_GT || op == TOK_GE ||
                op == TOK_AND || op == TOK_OR) {
                t.type_kind = TYPE_I32;
                t.type_id = TYPE_ID_I32;
            } else {
                t = resolve_type(node->ast_children[0]);
            }
            break;
        }

        case AST_UNARY:
            t = resolve_type(node->ast_children[0]);
            break;

        case AST_ARRAY_ACCESS: {
            Type arr = resolve_type(node->ast_children[0]);
            t = arr;
            /* An element is never itself an array.  Only class-reference elements
               (class or weak class) remain pointers; everything else is a value. */
            t.is_array = 0;
            t.array_size = 0;
            if (t.type_kind != TYPE_CLASS) {
                t.is_pointer = 0;
            }
            break;
        }

        case AST_MEMBER_ACCESS: {
            Type obj = resolve_type(node->ast_children[0]);
            ClassInfo* ci = NULL;
            if (obj.type_kind == TYPE_CLASS && obj.type_arg_count > 0) {
                ci = symtab_instantiate_class_from_type(&obj);
            } else {
                ci = symtab_find_class(obj.class_name);
            }
            if (ci) {
                int i;
                for (i = 0; i < ci->field_count; i++) {
                    if (strcmp(ci->field_names[i], node->ast_token.text) == 0) {
                        t = ci->field_types[i];
                        break;
                    }
                }
            } else {
                StructInfo* si = symtab_find_struct(obj.class_name);
                if (si) {
                    int i;
                    for (i = 0; i < si->field_count; i++) {
                        if (strcmp(si->field_names[i], node->ast_token.text) == 0) {
                            t = si->field_types[i];
                            break;
                        }
                    }
                }
            }
            break;
        }

        case AST_CALL: {
            if (node->ast_children[0]->ast_kind == AST_MEMBER_ACCESS) {
                AstNode* mem = node->ast_children[0];
                AstNode* obj = mem->ast_children[0];
                resolve_type(obj);
                if (strcmp(mem->ast_token.text, "lock") == 0 && obj->ast_resolved_type.is_weak) {
                    t = obj->ast_resolved_type;
                    t.is_weak = 0;
                    if (t.type_kind == TYPE_INTERFACE) {
                        t.is_pointer = 0;
                    } else {
                        t.is_pointer = 1;
                    }
                } else if (obj->ast_resolved_type.type_kind == TYPE_CLASS) {
                    ClassInfo* ci = NULL;
                    if (obj->ast_resolved_type.type_arg_count > 0) {
                        ci = symtab_instantiate_class_from_type(&obj->ast_resolved_type);
                    } else {
                        ci = symtab_find_class(obj->ast_resolved_type.class_name);
                    }
                    MethodInfo* mi = symtab_find_method_in_class(ci, mem->ast_token.text);
                    if (mi) { t = mi->return_type; }
                } else if (obj->ast_resolved_type.type_kind == TYPE_INTERFACE) {
                    InterfaceInfo* ii = symtab_find_interface(obj->ast_resolved_type.class_name);
                    if (ii) {
                        InterfaceMethodInfo* im = symtab_find_interface_method(ii, mem->ast_token.text);
                        if (im) { t = im->return_type; }
                    }
                }
            } else {
                FuncInfo* fi = symtab_find_func(node->ast_children[0]->ast_token.text);
                if (fi) { t = fi->return_type; }
            }
            break;
        }

        case AST_NEW: {
            t = node->ast_resolved_type;
            t.is_pointer = 1;
            break;
        }

        case AST_ASSIGN:
            t = resolve_type(node->ast_children[0]);
            break;

        case AST_VAR_DECL:
            t = node->ast_resolved_type;
            break;

        case AST_AS_CAST:
            resolve_type(node->ast_children[0]);
            t = node->ast_resolved_type;
            break;

        case AST_REF_ARG:
            t = resolve_type(node->ast_children[0]);
            break;

        default:
            break;
    }

    node->ast_resolved_type = t;
    return t;
}

static void codegen_expr(CodegenContext* ctx, AstNode* node, FILE* out);

static void codegen_binary(CodegenContext* ctx, AstNode* node, FILE* out) {
    TokenKind op = node->ast_token.kind;
    if (op == TOK_EQ || op == TOK_NE) {
        Type lt = resolve_type(node->ast_children[0]);
        Type rt = resolve_type(node->ast_children[1]);
        if (lt.type_kind == TYPE_INTERFACE && rt.type_kind == TYPE_INTERFACE) {
            fprintf(out, "(");
            codegen_expr(ctx, node->ast_children[0], out);
            fprintf(out, ".data %s ", node->ast_token.text);
            codegen_expr(ctx, node->ast_children[1], out);
            fprintf(out, ".data)");
            return;
        }
    }
    fprintf(out, "(");
    codegen_expr(ctx, node->ast_children[0], out);
    fprintf(out, " %s ", node->ast_token.text);
    codegen_expr(ctx, node->ast_children[1], out);
    fprintf(out, ")");
}

static void codegen_unary(CodegenContext* ctx, AstNode* node, FILE* out) {
    fprintf(out, "%s", node->ast_token.text);
    codegen_expr(ctx, node->ast_children[0], out);
}

static int type_is_ref(const Type* t) {
    return t->is_ref;
}

/* Emit a single argument.  If the callee expects a ref parameter, only a
   local variable identifier is allowed; it is passed as &var, or as the raw
   pointer if var itself is already a ref parameter. */
static void codegen_call_arg(CodegenContext* ctx, AstNode* arg, const Type* param_type, FILE* out) {
    if (param_type->is_array && !type_is_ref(param_type)) {
        fprintf(stderr, "error at %d:%d: array arguments must be passed by ref\n",
                arg->ast_token.line, arg->ast_token.col);
        fprintf(out, "/* invalid array arg */");
        return;
    }
    if (type_is_ref(param_type)) {
        if (arg->ast_kind != AST_REF_ARG) {
            fprintf(stderr, "error at %d:%d: missing 'ref' keyword for ref parameter\n",
                    arg->ast_token.line, arg->ast_token.col);
            ctx->codegen_error = 1;
            fprintf(out, "0 /* missing ref keyword */");
            return;
        }
        AstNode* var = arg->ast_children[0];
        if (!var || var->ast_kind != AST_IDENT) {
            fprintf(stderr, "error at %d:%d: ref argument must be a local variable\n",
                    arg->ast_token.line, arg->ast_token.col);
            fprintf(out, "0 /* invalid ref argument */");
            return;
        }
        SymEntry* e = symtab_lookup(var->ast_token.text);
        if (!e) {
            fprintf(stderr, "error at %d:%d: ref argument must be a local variable\n",
                    arg->ast_token.line, arg->ast_token.col);
            fprintf(out, "0 /* invalid ref argument */");
            return;
        }
        if (type_is_ref(&e->type)) {
            fprintf(out, "%s", var->ast_token.text);
        } else {
            fprintf(out, "&%s", var->ast_token.text);
        }
    } else if (arg->ast_kind == AST_REF_ARG) {
        fprintf(stderr, "error at %d:%d: 'ref' argument requires a ref parameter\n",
                arg->ast_token.line, arg->ast_token.col);
        ctx->codegen_error = 1;
        fprintf(out, "0 /* invalid ref argument */");
    } else if (param_type->is_weak && param_type->type_kind == TYPE_INTERFACE) {
        resolve_type(arg);
        Type rt = arg->ast_resolved_type;
        if (rt.is_weak && rt.type_kind == TYPE_INTERFACE) {
            /* weak interface -> weak interface: struct copy */
            codegen_expr(ctx, arg, out);
        } else if (rt.type_kind == TYPE_INTERFACE) {
            /* strong interface -> weak interface */
            if (arg->ast_kind == AST_CALL || arg->ast_kind == AST_NEW) {
                fprintf(out, "mylang_weakify_%s_owned(", param_type->class_name);
                codegen_expr(ctx, arg, out);
                fprintf(out, ")");
            } else {
                fprintf(out, "mylang_weakify_%s(", param_type->class_name);
                codegen_expr(ctx, arg, out);
                fprintf(out, ")");
            }
        } else if (rt.type_kind == TYPE_CLASS) {
            /* class -> weak interface */
            if (arg->ast_kind == AST_CALL || arg->ast_kind == AST_NEW) {
                fprintf(out, "mylang_weakify_%s_from_ptr_owned(", param_type->class_name);
                codegen_expr(ctx, arg, out);
                fprintf(out, ", &%s_%s_vtable)", rt.class_name, param_type->class_name);
            } else {
                fprintf(out, "mylang_weakify_%s_from_ptr(", param_type->class_name);
                codegen_expr(ctx, arg, out);
                fprintf(out, ", &%s_%s_vtable)", rt.class_name, param_type->class_name);
            }
        } else {
            fprintf(stderr, "error at %d:%d: cannot pass this argument to weak interface parameter\n",
                    arg->ast_token.line, arg->ast_token.col);
            fprintf(out, "/* invalid weak interface arg */");
        }
    } else if (param_type->is_weak) {
        resolve_type(arg);
        if (arg->ast_resolved_type.type_kind == TYPE_CLASS && !arg->ast_resolved_type.is_weak) {
            fprintf(out, "mylang_weak_init(");
            codegen_expr(ctx, arg, out);
            fprintf(out, ")");
        } else if (arg->ast_resolved_type.is_weak) {
            fprintf(out, "mylang_weak_copy(");
            codegen_expr(ctx, arg, out);
            fprintf(out, ")");
        } else {
            codegen_expr(ctx, arg, out);
        }
    } else {
        codegen_expr(ctx, arg, out);
    }
}

static int is_array_method_name(const char* s) {
    return strcmp(s, "push") == 0 || strcmp(s, "pop") == 0 ||
           strcmp(s, "reserve") == 0 || strcmp(s, "resize") == 0 ||
           strcmp(s, "clear") == 0 || strcmp(s, "compact") == 0 ||
           strcmp(s, "move_to") == 0 || strcmp(s, "copy_to") == 0;
}

static void emit_array_ref_arg(CodegenContext* ctx, AstNode* arg, FILE* out) {
    if (!arg || arg->ast_kind != AST_IDENT) {
        fprintf(stderr, "error at %d:%d: array move/copy destination must be a local variable\n",
                arg ? arg->ast_token.line : 0, arg ? arg->ast_token.col : 0);
        ctx->codegen_error = 1;
        fprintf(out, "/* invalid array destination */");
        return;
    }
    SymEntry* e = symtab_lookup(arg->ast_token.text);
    if (!e) {
        fprintf(stderr, "error at %d:%d: array move/copy destination must be a local variable\n",
                arg->ast_token.line, arg->ast_token.col);
        ctx->codegen_error = 1;
        fprintf(out, "/* invalid array destination */");
        return;
    }
    if (type_is_ref(&e->type)) {
        fprintf(out, "%s", arg->ast_token.text);
    } else {
        fprintf(out, "&%s", arg->ast_token.text);
    }
}

static void codegen_array_method_call(CodegenContext* ctx, AstNode* arr,
                                      const char* mname, AstNode* args, FILE* out) {
    Type arr_type = arr->ast_resolved_type;
    char elem_size[128];
    char elem_type[128];
    array_elem_size_expr(&arr_type, elem_size, sizeof(elem_size));
    c_array_elem_type_name(&arr_type, elem_type, sizeof(elem_type));
    int kind = array_elem_kind(&arr_type);

    if (strcmp(mname, "push") == 0) {
        if (!args) {
            fprintf(stderr, "error at %d:%d: push() requires a value argument\n",
                    arr->ast_token.line, arr->ast_token.col);
            ctx->codegen_error = 1;
            fprintf(out, "0 /* missing push value */");
            return;
        }
        fprintf(out, "mylang_array_push(");
        emit_array_ptr_expr(ctx, arr, out);
        fprintf(out, ", %s, %d, (%s[]){", elem_size, kind, elem_type);
        codegen_expr(ctx, args, out);
        fprintf(out, "})");
    } else if (strcmp(mname, "pop") == 0) {
        fprintf(out, "mylang_array_pop(");
        emit_array_ptr_expr(ctx, arr, out);
        fprintf(out, ", %s, %d)", elem_size, kind);
    } else if (strcmp(mname, "reserve") == 0) {
        if (!args) {
            fprintf(stderr, "error at %d:%d: reserve() requires a capacity argument\n",
                    arr->ast_token.line, arr->ast_token.col);
            ctx->codegen_error = 1;
            fprintf(out, "0 /* missing reserve capacity */");
            return;
        }
        fprintf(out, "mylang_array_reserve(");
        emit_array_ptr_expr(ctx, arr, out);
        fprintf(out, ", (size_t)(");
        codegen_expr(ctx, args, out);
        fprintf(out, "), %s)", elem_size);
    } else if (strcmp(mname, "resize") == 0) {
        if (!args) {
            fprintf(stderr, "error at %d:%d: resize() requires a length argument\n",
                    arr->ast_token.line, arr->ast_token.col);
            ctx->codegen_error = 1;
            fprintf(out, "0 /* missing resize length */");
            return;
        }
        fprintf(out, "mylang_array_resize(");
        emit_array_ptr_expr(ctx, arr, out);
        fprintf(out, ", (size_t)(");
        codegen_expr(ctx, args, out);
        fprintf(out, "), %s, %d)", elem_size, kind);
    } else if (strcmp(mname, "clear") == 0) {
        fprintf(out, "mylang_array_clear(");
        emit_array_ptr_expr(ctx, arr, out);
        fprintf(out, ", %s, %d)", elem_size, kind);
    } else if (strcmp(mname, "compact") == 0) {
        fprintf(out, "mylang_array_compact(");
        emit_array_ptr_expr(ctx, arr, out);
        fprintf(out, ", %s)", elem_size);
    } else if (strcmp(mname, "move_to") == 0) {
        if (!args || args->ast_kind != AST_REF_ARG) {
            fprintf(stderr, "error at %d:%d: move_to() requires a ref destination argument\n",
                    arr->ast_token.line, arr->ast_token.col);
            ctx->codegen_error = 1;
            fprintf(out, "0 /* missing move_to destination */");
            return;
        }
        fprintf(out, "mylang_array_move(");
        emit_array_ptr_expr(ctx, arr, out);
        fprintf(out, ", ");
        emit_array_ref_arg(ctx, args->ast_children[0], out);
        fprintf(out, ", %s, %d)", elem_size, kind);
    } else if (strcmp(mname, "copy_to") == 0) {
        if (!args || args->ast_kind != AST_REF_ARG) {
            fprintf(stderr, "error at %d:%d: copy_to() requires a ref destination argument\n",
                    arr->ast_token.line, arr->ast_token.col);
            ctx->codegen_error = 1;
            fprintf(out, "0 /* missing copy_to destination */");
            return;
        }
        fprintf(out, "mylang_array_copy(");
        emit_array_ptr_expr(ctx, arr, out);
        fprintf(out, ", ");
        emit_array_ref_arg(ctx, args->ast_children[0], out);
        fprintf(out, ", %s, %d)", elem_size, kind);
    } else {
        fprintf(stderr, "error at %d:%d: unknown array method '%s'\n",
                arr->ast_token.line, arr->ast_token.col, mname);
        ctx->codegen_error = 1;
        fprintf(out, "0 /* unknown array method */");
    }
}

static void codegen_call(CodegenContext* ctx, AstNode* node, FILE* out) {
    /* method call: p.foo(...) ClassName_foo(p, ...) */
    if (node->ast_children[0]->ast_kind == AST_MEMBER_ACCESS) {
        AstNode* mem = node->ast_children[0];
        AstNode* obj = mem->ast_children[0];
        const char* mname = mem->ast_token.text;
        resolve_type(obj);
        AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;

        if (obj->ast_resolved_type.is_array && is_array_method_name(mname)) {
            codegen_array_method_call(ctx, obj, mname, args, out);
            return;
        }

        if (strcmp(mname, "lock") == 0 && obj->ast_resolved_type.is_weak) {
            if (obj->ast_resolved_type.type_kind == TYPE_INTERFACE) {
                fprintf(out, "mylang_lock_%s(", obj->ast_resolved_type.class_name);
                codegen_expr(ctx, obj, out);
                fprintf(out, ".wr, ");
                codegen_expr(ctx, obj, out);
                fprintf(out, ".vt)");
            } else {
                fprintf(out, "mylang_lock(");
                codegen_expr(ctx, obj, out);
                fprintf(out, ")");
            }
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
            const char* class_c = ci ? class_c_name(ci) : obj->ast_resolved_type.class_name;
            fprintf(out, "%s_%s(", class_c, mname);
            codegen_expr(ctx, obj, out);
            int idx = 0;
            while (args) {
                fprintf(out, ", ");
                Type expected;
                memset(&expected, 0, sizeof(expected));
                if (mi && idx < mi->param_count) expected = mi->param_types[idx];
                codegen_call_arg(ctx, args, &expected, out);
                idx++;
                args = args->next;
            }
            fprintf(out, ")");
            return;
        }
        if (obj->ast_resolved_type.type_kind == TYPE_INTERFACE) {
            InterfaceInfo* ii = symtab_find_interface(obj->ast_resolved_type.class_name);
            InterfaceMethodInfo* im = NULL;
            if (ii) im = symtab_find_interface_method(ii, mname);

            fprintf(out, "(");
            codegen_expr(ctx, obj, out);
            fprintf(out, ").vtable->%s((", mname);
            codegen_expr(ctx, obj, out);
            fprintf(out, ").data");

            int idx = 0;
            while (args) {
                fprintf(out, ", ");
                Type expected;
                memset(&expected, 0, sizeof(expected));
                if (im && idx < im->param_count) expected = im->param_types[idx];
                codegen_call_arg(ctx, args, &expected, out);
                idx++;
                args = args->next;
            }
            fprintf(out, ")");
            return;
        }
    }
    /* normal function call */
    AstNode* callee = node->ast_children[0];
    FuncInfo* fi = NULL;
    if (callee->ast_kind == AST_IDENT) {
        fi = symtab_find_func(callee->ast_token.text);
    }
    codegen_expr(ctx, callee, out);
    fprintf(out, "(");
    AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
    int first = 1;
    int idx = 0;
    while (args) {
        if (!first) fprintf(out, ", ");
        Type expected;
        memset(&expected, 0, sizeof(expected));
        if (fi && idx < fi->param_count) expected = fi->param_types[idx];
        codegen_call_arg(ctx, args, &expected, out);
        first = 0;
        idx++;
        args = args->next;
    }
    fprintf(out, ")");
}

static void emit_array_ptr_expr(CodegenContext* ctx, AstNode* arr_node, FILE* out) {
    /* codegen_expr yields the MyArray value; take its address for helpers. */
    fprintf(out, "&(");
    codegen_expr(ctx, arr_node, out);
    fprintf(out, ")");
}

static void codegen_array_access(CodegenContext* ctx, AstNode* node, FILE* out) {
    AstNode* arr = node->ast_children[0];
    AstNode* idx = node->ast_children[1];
    char elem_type[128];
    char elem_size[128];
    resolve_type(arr);
    (void)resolve_type(node);
    c_array_elem_type_name(&arr->ast_resolved_type, elem_type, sizeof(elem_type));
    array_elem_size_expr(&arr->ast_resolved_type, elem_size, sizeof(elem_size));
    fprintf(out, "(*(%s*)mylang_array_at(", elem_type);
    emit_array_ptr_expr(ctx, arr, out);
    fprintf(out, ", ");
    codegen_expr(ctx, idx, out);
    fprintf(out, ", %s, \"%s\", %d))", elem_size, ctx->source_file_escaped, node->ast_token.line);
}

static void codegen_member_access(CodegenContext* ctx, AstNode* node, FILE* out) {
    AstNode* obj = node->ast_children[0];
    resolve_type(obj);
    if (obj->ast_resolved_type.is_array) {
        /* Arrays are value-type MyArray structs; member access always uses '.'. */
        codegen_expr(ctx, obj, out);
        fprintf(out, ".%s", node->ast_token.text);
    } else if (obj->ast_resolved_type.type_kind == TYPE_CLASS) {
        /* Class references may come from void* getters (e.g. dynamic arrays),
           so cast to the concrete struct pointer before using ->. */
        fprintf(out, "((%s*)", c_base_name(&obj->ast_resolved_type));
        codegen_expr(ctx, obj, out);
        fprintf(out, ")->%s", node->ast_token.text);
    } else if (obj->ast_resolved_type.is_pointer) {
        codegen_expr(ctx, obj, out);
        fprintf(out, "->%s", node->ast_token.text);
    } else {
        codegen_expr(ctx, obj, out);
        fprintf(out, ".%s", node->ast_token.text);
    }
}

static void codegen_new(CodegenContext* ctx, AstNode* node, FILE* out) {
    Type base = node->ast_resolved_type;
    if (base.type_kind == TYPE_CLASS && base.type_arg_count > 0) {
        symtab_instantiate_class_from_type(&base);
    }
    if (node->ast_child_count > 0) {
        /* Parser rejects 'new T[N]'; arrays are created empty and grown with
           push/reserve.  This branch is only reached on parse-error recovery. */
        fprintf(stderr, "error at %d:%d: 'new %s[...]' is not supported; use '%s[] name; name.reserve(size)'\n",
                node->ast_token.line, node->ast_token.col, base.class_name, base.class_name);
        ctx->codegen_error = 1;
        fprintf(out, "/* invalid new array */");
    } else {
        if (base.type_kind == TYPE_CLASS) {
            char dtor_name[128];
            ClassInfo* ci = symtab_find_class(base.class_name);
            if (!ci) ci = symtab_find_class_by_mangled(base.class_name);
            if (ci && ci->mangled_name[0]) {
                snprintf(dtor_name, sizeof(dtor_name), "_mylang_dtor_%s", ci->mangled_name);
            } else {
                snprintf(dtor_name, sizeof(dtor_name), "_mylang_dtor_%s", c_base_name(&base));
            }
            fprintf(out, "mylang_new_object(sizeof(%s), %u, %s)", c_base_name(&base), (unsigned)base.type_id, dtor_name);
        } else {
            fprintf(out, "calloc(1, sizeof(%s))", c_base_name(&base));
        }
    }
    node->ast_resolved_type = base;
}

static void codegen_char_lit(CodegenContext* ctx, AstNode* node, FILE* out) {
    char c = node->ast_token.char_val;
    switch (c) {
        case '\n': fprintf(out, "'\\n'"); break;
        case '\t': fprintf(out, "'\\t'"); break;
        case '\r': fprintf(out, "'\\r'"); break;
        case '\\': fprintf(out, "'\\\\'"); break;
        case '\'': fprintf(out, "'\\''"); break;
        default:   fprintf(out, "'%c'", c); break;
    }
}

static void codegen_expr(CodegenContext* ctx, AstNode* node, FILE* out) {
    if (!node) return;
    resolve_type(node);

    if (node->ast_temp_name[0] != '\0') {
        fprintf(out, "%s", node->ast_temp_name);
        return;
    }

    switch (node->ast_kind) {
        case AST_INT_LIT:
            fprintf(out, "%d", node->ast_token.int_val);
            break;
        case AST_CHAR_LIT:
            codegen_char_lit(ctx, node, out);
            break;
        case AST_IDENT: {
            if (strcmp(node->ast_token.text, "this") == 0) {
                fprintf(out, "thiz");
            } else {
                SymEntry* e = symtab_lookup(node->ast_token.text);
                if (e && type_is_ref(&e->type)) {
                    fprintf(out, "(*%s)", node->ast_token.text);
                } else {
                    fprintf(out, "%s", node->ast_token.text);
                }
            }
            break;
        }
        case AST_BINARY:
            codegen_binary(ctx, node, out);
            break;
        case AST_UNARY:
            codegen_unary(ctx, node, out);
            break;
        case AST_ASSIGN: {
            AstNode* lhs = node->ast_children[0];
            AstNode* rhs = node->ast_children[1];
            resolve_type(lhs);
            Type lt = lhs->ast_resolved_type;

            if (lt.is_array) {
                fprintf(stderr, "error at %d:%d: cannot assign arrays directly; use move_to(ref) or copy_to(ref)\n",
                        lhs->ast_token.line, lhs->ast_token.col);
                ctx->codegen_error = 1;
                fprintf(out, "0 /* invalid array assignment */");
                break;
            }

            if (lhs->ast_kind == AST_ARRAY_ACCESS) {
                Type at = lhs->ast_children[0]->ast_resolved_type;
                if (at.is_array) {
                    /* array element assignment */
                    int rhs_owned = (rhs->ast_kind == AST_CALL || rhs->ast_kind == AST_NEW);
                    resolve_type(rhs);
                    Type rt = rhs->ast_resolved_type;

                    if (lt.type_kind == TYPE_CLASS && !lt.is_weak) {
                        fprintf(out, "((void)mylang_release(");
                        codegen_array_access(ctx, lhs, out);
                        fprintf(out, "), ");
                        codegen_array_access(ctx, lhs, out);
                        fprintf(out, " = ");
                        if (rhs_owned) {
                            codegen_expr(ctx, rhs, out);
                        } else {
                            fprintf(out, "mylang_retain(");
                            codegen_expr(ctx, rhs, out);
                            fprintf(out, ")");
                        }
                        fprintf(out, ")");
                    } else if (lt.type_kind == TYPE_INTERFACE && !lt.is_weak) {
                        fprintf(out, "((void)mylang_release(");
                        codegen_array_access(ctx, lhs, out);
                        fprintf(out, ".data), ");
                        codegen_array_access(ctx, lhs, out);
                        fprintf(out, " = ");
                        if (rt.type_kind == TYPE_CLASS) {
                            fprintf(out, "(%s){ ", lt.class_name);
                            if (!rhs_owned) {
                                fprintf(out, "mylang_retain(");
                            }
                            codegen_expr(ctx, rhs, out);
                            if (!rhs_owned) {
                                fprintf(out, ")");
                            }
                            fprintf(out, ", &%s_%s_vtable }", rt.class_name, lt.class_name);
                        } else if (rhs_owned) {
                            codegen_expr(ctx, rhs, out);
                        } else {
                            fprintf(out, "mylang_retain(");
                            codegen_expr(ctx, rhs, out);
                            fprintf(out, ".data), ");
                            codegen_expr(ctx, rhs, out);
                        }
                        fprintf(out, ")");
                    } else if (lt.is_weak && lt.type_kind == TYPE_INTERFACE) {
                        fprintf(out, "((void)mylang_weak_release(");
                        codegen_array_access(ctx, lhs, out);
                        fprintf(out, ".wr), ");
                        codegen_array_access(ctx, lhs, out);
                        fprintf(out, " = ");
                        if (rt.is_weak && rt.type_kind == TYPE_INTERFACE) {
                            /* weak-to-weak: copy the whole WeakIFoo value */
                            codegen_expr(ctx, rhs, out);
                        } else if (rt.type_kind == TYPE_INTERFACE) {
                            /* strong interface -> weak interface */
                            if (rhs_owned) {
                                fprintf(out, "mylang_weakify_%s_owned(", lt.class_name);
                            } else {
                                fprintf(out, "mylang_weakify_%s(", lt.class_name);
                            }
                            codegen_expr(ctx, rhs, out);
                            fprintf(out, ")");
                        } else if (rt.type_kind == TYPE_CLASS) {
                            /* class -> weak interface */
                            if (rhs_owned) {
                                fprintf(out, "mylang_weakify_%s_from_ptr_owned(", lt.class_name);
                            } else {
                                fprintf(out, "mylang_weakify_%s_from_ptr(", lt.class_name);
                            }
                            codegen_expr(ctx, rhs, out);
                            fprintf(out, ", &%s_%s_vtable)", rt.class_name, lt.class_name);
                        }
                        fprintf(out, ")");
                    } else if (lt.is_weak) {
                        /* weak class element */
                        fprintf(out, "((void)mylang_weak_release(");
                        codegen_array_access(ctx, lhs, out);
                        fprintf(out, "), ");
                        codegen_array_access(ctx, lhs, out);
                        fprintf(out, " = ");
                        if (rhs_owned) {
                            fprintf(out, "mylang_weak_init(");
                            codegen_expr(ctx, rhs, out);
                            fprintf(out, ")");
                        } else {
                            codegen_expr(ctx, rhs, out);
                        }
                        fprintf(out, ")");
                    } else {
                        codegen_array_access(ctx, lhs, out);
                        fprintf(out, " = ");
                        codegen_expr(ctx, rhs, out);
                    }
                    break;
                }
            }

            if (lt.is_weak && lt.type_kind == TYPE_CLASS) {
                /* Weak class field: old WeakRef is released; RHS is weak-copied
                   or weakified. */
                resolve_type(rhs);
                Type rt = rhs->ast_resolved_type;
                int rhs_owned = (rhs->ast_kind == AST_CALL || rhs->ast_kind == AST_NEW);

                fprintf(out, "((void)mylang_weak_release(");
                codegen_expr(ctx, lhs, out);
                fprintf(out, "), ");
                if (rt.is_weak) {
                    fprintf(out, "(");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, " = mylang_weak_copy(");
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, "))");
                } else if (rhs_owned) {
                    int tmp_id = ctx->assign_tmp_id++;
                    fprintf(out, "(void* _wassign%d = ", tmp_id);
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ", ");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, " = mylang_weak_init(_wassign%d), mylang_release(_wassign%d))",
                            tmp_id, tmp_id);
                } else {
                    fprintf(out, "(");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, " = mylang_weak_init(");
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, "))");
                }
                fprintf(out, ")");
            } else if (lt.type_kind == TYPE_CLASS) {
                int rhs_owned = (rhs->ast_kind == AST_CALL || rhs->ast_kind == AST_NEW);
                int rhs_local = (rhs->ast_kind == AST_IDENT && symtab_lookup(rhs->ast_token.text) != NULL);
                /* Fields own their class references, so assigning a local or
                   parameter to a field must retain the source.  Local-to-local
                   assignment keeps the existing borrow optimization. */
                int lhs_is_field = (lhs->ast_kind == AST_MEMBER_ACCESS);

                fprintf(out, "((");
                if (!rhs_owned && (!rhs_local || lhs_is_field)) {
                    fprintf(out, "(void)mylang_retain(");
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, "), ");
                }
                fprintf(out, "(void)mylang_release(");
                codegen_expr(ctx, lhs, out);
                fprintf(out, "), (");
                codegen_expr(ctx, lhs, out);
                fprintf(out, " = ");
                codegen_expr(ctx, rhs, out);
                fprintf(out, ")))");
            } else if (lt.is_weak && lt.type_kind == TYPE_INTERFACE) {
                resolve_type(rhs);
                Type rt = rhs->ast_resolved_type;
                int rhs_owned = (rhs->ast_kind == AST_CALL || rhs->ast_kind == AST_NEW);

                /* release old weak ref first */
                fprintf(out, "((void)mylang_weak_release(");
                codegen_expr(ctx, lhs, out);
                fprintf(out, ".wr), ");

                if (rt.is_weak && rt.type_kind == TYPE_INTERFACE) {
                    /* weak -> weak copy */
                    fprintf(out, "(");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, ".wr = mylang_weak_copy(");
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ".wr), ");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, ".vt = ");
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ".vt))");
                } else if (rt.type_kind == TYPE_INTERFACE) {
                    if (rhs_owned) {
                        int tmp_id = ctx->assign_tmp_id++;
                        fprintf(out, "(%s _wassign%d = ", lt.class_name, tmp_id);
                        codegen_expr(ctx, rhs, out);
                        fprintf(out, ", ");
                        codegen_expr(ctx, lhs, out);
                        fprintf(out, ".wr = mylang_weak_init(_wassign%d.data), ", tmp_id);
                        codegen_expr(ctx, lhs, out);
                        fprintf(out, ".vt = _wassign%d.vtable, mylang_release(_wassign%d.data))", tmp_id, tmp_id);
                    } else {
                        fprintf(out, "(");
                        codegen_expr(ctx, lhs, out);
                        fprintf(out, ".wr = mylang_weak_init(");
                        codegen_expr(ctx, rhs, out);
                        fprintf(out, ".data), ");
                        codegen_expr(ctx, lhs, out);
                        fprintf(out, ".vt = ");
                        codegen_expr(ctx, rhs, out);
                        fprintf(out, ".vtable))");
                    }
                } else if (rt.type_kind == TYPE_CLASS) {
                    if (rhs_owned) {
                        int tmp_id = ctx->assign_tmp_id++;
                        fprintf(out, "(void* _wassign%d = ", tmp_id);
                        codegen_expr(ctx, rhs, out);
                        fprintf(out, ", ");
                        codegen_expr(ctx, lhs, out);
                        fprintf(out, ".wr = mylang_weak_init(_wassign%d), ", tmp_id);
                        codegen_expr(ctx, lhs, out);
                        fprintf(out, ".vt = &%s_%s_vtable, mylang_release(_wassign%d))",
                                rt.class_name, lt.class_name, tmp_id);
                    } else {
                        fprintf(out, "(");
                        codegen_expr(ctx, lhs, out);
                        fprintf(out, ".wr = mylang_weak_init(");
                        codegen_expr(ctx, rhs, out);
                        fprintf(out, "), ");
                        codegen_expr(ctx, lhs, out);
                        fprintf(out, ".vt = &%s_%s_vtable))",
                                rt.class_name, lt.class_name);
                    }
                } else {
                    fprintf(stderr, "error at %d:%d: cannot assign to weak interface from this type\n",
                            node->ast_token.line, node->ast_token.col);
                    ctx->codegen_error = 1;
                    fprintf(out, "0)");
                }
            } else if (lt.type_kind == TYPE_INTERFACE) {
                resolve_type(rhs);
                Type rt = rhs->ast_resolved_type;
                int rhs_owned = (rhs->ast_kind == AST_CALL || rhs->ast_kind == AST_NEW);

                fprintf(out, "((void)");
                if (rt.type_kind == TYPE_CLASS) {
                    if (!rhs_owned) {
                        fprintf(out, "mylang_retain(");
                        codegen_expr(ctx, rhs, out);
                        fprintf(out, "), ");
                    }
                    fprintf(out, "mylang_release(");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, ".data), (");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, ".data = ");
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ", ");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, ".vtable = &%s_%s_vtable", rt.class_name, lt.class_name);
                    fprintf(out, "))");
                } else {
                    if (!rhs_owned) {
                        fprintf(out, "mylang_retain(");
                        codegen_expr(ctx, rhs, out);
                        fprintf(out, ".data), ");
                    }
                    fprintf(out, "mylang_release(");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, ".data), (");
                    codegen_expr(ctx, lhs, out);
                    fprintf(out, " = ");
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ")))");
                }
            } else {
                codegen_expr(ctx, lhs, out);
                fprintf(out, " = ");
                codegen_expr(ctx, rhs, out);
            }
            break;
        }
        case AST_CALL:
            codegen_call(ctx, node, out);
            break;
        case AST_ARRAY_ACCESS:
            codegen_array_access(ctx, node, out);
            break;
        case AST_MEMBER_ACCESS:
            codegen_member_access(ctx, node, out);
            break;
        case AST_NEW:
            codegen_new(ctx, node, out);
            break;
        case AST_AS_CAST: {
            AstNode* obj = node->ast_children[0];
            resolve_type(obj);
            Type target = node->ast_resolved_type;
            if (target.type_kind == TYPE_CLASS) {
                ClassInfo* ci = symtab_find_class(target.class_name);
                /* Use a temporary if the object expression was hoisted by
                   emit_subexpr_temps; otherwise codegen_expr would re-evaluate
                   a call-like subexpression (e.g. w.lock()) twice. */
                const char* obj_name = (obj->ast_temp_name[0] != '\0') ? obj->ast_temp_name : NULL;
                fprintf(out, "((");
                if (obj_name) fprintf(out, "%s", obj_name);
                else codegen_expr(ctx, obj, out);
                fprintf(out, ").vtable->concrete_type_id == %u ? (%s*)(",
                        ci ? (unsigned)ci->type_id : 0, target.class_name);
                if (obj_name) fprintf(out, "%s", obj_name);
                else codegen_expr(ctx, obj, out);
                fprintf(out, ").data : NULL)");
            } else {
                fprintf(stderr, "error at %d:%d: 'as' target must be a class type\n",
                        node->ast_token.line, node->ast_token.col);
                ctx->codegen_error = 1;
                fprintf(out, "/* as-cast unsupported target */");
            }
            break;
        }
        default:
            fprintf(out, "/* ??? */");
            break;
    }
}

/* -- bounds checking ------------------------------------------------- */

static void indent_line(FILE* out, int indent);

static void emit_bounds_checks(CodegenContext* ctx, AstNode* expr, FILE* out, int indent) {
    if (!expr) return;

    /* Dynamic arrays perform bounds checks inside mylang_array_at.  Fixed-size
       arrays have been removed from the language. */
    (void)ctx;
    (void)out;
    (void)indent;

    int i;
    for (i = 0; i < expr->ast_child_count; i++) {
        emit_bounds_checks(ctx, expr->ast_children[i], out, indent);
    }
    emit_bounds_checks(ctx, expr->next, out, indent);
}

/* -- caller-side arg guard helpers ----------------------------------- */


static int call_needs_guard(AstNode* arg) {
    if (!arg) return 0;
    if (arg->ast_kind == AST_REF_ARG) return 0;
    resolve_type(arg);
    TypeKind k = arg->ast_resolved_type.type_kind;
    if (k != TYPE_CLASS && k != TYPE_INTERFACE) return 0;
    if (arg->ast_resolved_type.is_array) return 0;
    if (arg->ast_kind == AST_ASSIGN) return 0;
    if (arg->ast_kind == AST_IDENT && symtab_lookup(arg->ast_token.text)) return 0;
    return 1;
}

static int guard_needs_retain(AstNode* node) {
    /* Calls/new already return an owned (+1) reference. */
    return node->ast_kind != AST_CALL && node->ast_kind != AST_NEW;
}

static int return_expr_needs_retain(AstNode* node) {
    /* Calls/new already produce an owned (+1) reference for the caller.
       Local variables, fields, and array elements must be retained because
       the caller will release the returned value and the original owner still
       holds its own reference. */
    return node->ast_kind != AST_CALL && node->ast_kind != AST_NEW;
}

/* Sub-expressions that must be hoisted into a temporary so they are evaluated
   exactly once.  Only call sites can add reference counts; lvalues, weak refs,
   arrays and 'new' are left to their surrounding statement. */
static int subexpr_needs_temp(AstNode* node) {
    if (!node) return 0;
    resolve_type(node);
    Type* t = &node->ast_resolved_type;
    if (t->is_weak) return 0;
    if (t->is_array) return 0;
    if (t->type_kind != TYPE_CLASS && t->type_kind != TYPE_INTERFACE) return 0;
    return node->ast_kind == AST_CALL;
}

/* Evaluate each guarded class subexpression into a temporary once, so we do
   not re-evaluate side-effecting expressions (e.g. method calls) when the
   caller-side retain/release guards are emitted. */
static void emit_guarded_temp_decls(CodegenContext* ctx, AstNode* expr, FILE* out, int indent) {
    if (!expr) return;

    /* Do not extract the LHS of an assignment into a temporary; it must remain
       an lvalue so the assignment writes to the real location. */
    if (expr->ast_kind == AST_ASSIGN) {
        emit_guarded_temp_decls(ctx, expr->ast_children[1], out, indent);
        emit_guarded_temp_decls(ctx, expr->next, out, indent);
        return;
    }

    int i;
    for (i = 0; i < expr->ast_child_count; i++) {
        emit_guarded_temp_decls(ctx, expr->ast_children[i], out, indent);
    }
    emit_guarded_temp_decls(ctx, expr->next, out, indent);

    if (call_needs_guard(expr) && expr->ast_temp_name[0] == '\0') {
        int id = ctx->guard_tmp_id++;
        int n = snprintf(expr->ast_temp_name, sizeof(expr->ast_temp_name), "_g%d", id);
        CHECK_SNPRINTF(n, sizeof(expr->ast_temp_name), "guard temporary name too long");

        char tbuf[128];
        c_type_str(&expr->ast_resolved_type, tbuf, sizeof(tbuf));
        indent_line(out, indent);
        fprintf(out, "%s %s = ", tbuf, expr->ast_temp_name);

        /* Evaluate the original expression without using its own temp name. */
        char saved[64];
        CHECK_STRSCPY(strscpy(saved, expr->ast_temp_name, sizeof(saved)),
                      "guard temporary name too long");
        expr->ast_temp_name[0] = '\0';
        codegen_expr(ctx, expr, out);
        CHECK_STRSCPY(strscpy(expr->ast_temp_name, saved, sizeof(expr->ast_temp_name)),
                      "guard temporary name too long");

        fprintf(out, ";\n");
    }
}

static void cleanup_add(CodegenContext* ctx, const char* name, int is_weak, int is_interface);

/* Extract an owned class/interface call into a temporary variable so it is
   evaluated exactly once.  The temporary is released via cleanup. */
static void extract_owned_call_temp(CodegenContext* ctx, AstNode* node, FILE* out, int indent) {
    if (!node || node->ast_temp_name[0] != '\0') return;
    if (!subexpr_needs_temp(node)) return;

    int id = ctx->subexpr_tmp_id++;
    int n = snprintf(node->ast_temp_name, sizeof(node->ast_temp_name), "_i%d", id);
    CHECK_SNPRINTF(n, sizeof(node->ast_temp_name), "subexpr temporary name too long");

    char tbuf[128];
    c_type_str(&node->ast_resolved_type, tbuf, sizeof(tbuf));
    indent_line(out, indent);
    fprintf(out, "%s %s = ", tbuf, node->ast_temp_name);

    /* Evaluate the original expression without using its own temp name. */
    char saved[64];
    CHECK_STRSCPY(strscpy(saved, node->ast_temp_name, sizeof(saved)),
                  "subexpr temporary name too long");
    node->ast_temp_name[0] = '\0';
    codegen_expr(ctx, node, out);
    CHECK_STRSCPY(strscpy(node->ast_temp_name, saved, sizeof(node->ast_temp_name)),
                  "subexpr temporary name too long");

    fprintf(out, ";\n");

    if (node->ast_resolved_type.type_kind == TYPE_INTERFACE) {
        cleanup_add(ctx, node->ast_temp_name, 0, 1);
    } else {
        cleanup_add(ctx, node->ast_temp_name, 0, 0);
    }
}

/* Extract owned class/interface subexpression calls into temporaries so they
   are not re-evaluated (which leaks reference counts).  Direct call arguments
   are left for emit_guarded_temp_decls to handle. */
static void emit_subexpr_temps_impl(CodegenContext* ctx, AstNode* node, FILE* out, int indent, int extract_root) {
    if (!node) return;

    if (node->ast_kind == AST_AS_CAST) {
        emit_subexpr_temps_impl(ctx, node->ast_children[0], out, indent, 1);
        AstNode* obj = node->ast_children[0];
        if (subexpr_needs_temp(obj)) {
            extract_owned_call_temp(ctx, obj, out, indent);
        }
        emit_subexpr_temps_impl(ctx, node->next, out, indent, 1);
        return;
    }

    if (node->ast_kind == AST_BINARY) {
        int i;
        for (i = 0; i < node->ast_child_count; i++) {
            emit_subexpr_temps_impl(ctx, node->ast_children[i], out, indent, 1);
        }
        for (i = 0; i < node->ast_child_count; i++) {
            AstNode* child = node->ast_children[i];
            if (subexpr_needs_temp(child)) {
                extract_owned_call_temp(ctx, child, out, indent);
            }
        }
        emit_subexpr_temps_impl(ctx, node->next, out, indent, 1);
        return;
    }

    if (node->ast_kind == AST_MEMBER_ACCESS) {
        emit_subexpr_temps_impl(ctx, node->ast_children[0], out, indent, 1);
        AstNode* obj = node->ast_children[0];
        if (subexpr_needs_temp(obj)) {
            extract_owned_call_temp(ctx, obj, out, indent);
        }
        emit_subexpr_temps_impl(ctx, node->next, out, indent, 1);
        return;
    }

    if (node->ast_kind == AST_CALL) {
        /* Recurse into callee for method-call receiver subexpressions. */
        if (node->ast_children[0]->ast_kind == AST_MEMBER_ACCESS) {
            emit_subexpr_temps_impl(ctx, node->ast_children[0], out, indent, 1);
        }
        /* Top-level arguments are handled by emit_guarded_temp_decls for
           lifetime management.  However, nested subexpressions inside
           arguments (e.g. the object of an 'as' cast or an interface method
           receiver) may be evaluated multiple times by their parent, so we
           extract those into temporaries.  We pass extract_root=0 so the
           argument root itself is not extracted here. */
        AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
        while (args) {
            emit_subexpr_temps_impl(ctx, args, out, indent, 0);
            args = args->next;
        }
        emit_subexpr_temps_impl(ctx, node->next, out, indent, 1);
        return;
    }

    if (node->ast_kind == AST_ASSIGN) {
        /* LHS must remain an lvalue.  RHS is evaluated into its destination,
           so only nested subexpressions inside RHS need extraction. */
        emit_subexpr_temps_impl(ctx, node->ast_children[1], out, indent, 1);
        emit_subexpr_temps_impl(ctx, node->next, out, indent, 1);
        return;
    }

    if (node->ast_kind == AST_NEW) {
        /* 'new' is handled by its surrounding statement; only nested size
           expressions need subexpression extraction. */
        emit_subexpr_temps_impl(ctx, node->ast_children[0], out, indent, 1);
        emit_subexpr_temps_impl(ctx, node->next, out, indent, 1);
        return;
    }

    int i;
    for (i = 0; i < node->ast_child_count; i++) {
        emit_subexpr_temps_impl(ctx, node->ast_children[i], out, indent, 1);
    }
    emit_subexpr_temps_impl(ctx, node->next, out, indent, 1);

    if (extract_root && subexpr_needs_temp(node)) {
        extract_owned_call_temp(ctx, node, out, indent);
    }
}

static void emit_subexpr_temps(CodegenContext* ctx, AstNode* node, FILE* out, int indent) {
    emit_subexpr_temps_impl(ctx, node, out, indent, 1);
}

static void emit_call_guards(CodegenContext* ctx, AstNode* expr, FILE* out, int is_retain) {
    if (!expr) return;
    if (expr->ast_kind == AST_CALL) {
        AstNode* callee = expr->ast_children[0];
        AstNode* args  = (expr->ast_child_count > 1) ? expr->ast_children[1] : NULL;

        if (callee->ast_kind == AST_MEMBER_ACCESS) {
            AstNode* obj = callee->ast_children[0];
            if (call_needs_guard(obj)) {
                if (is_retain) {
                    if (guard_needs_retain(obj)) fprintf(out, "mylang_retain(%s); ", obj->ast_temp_name);
                } else {
                    fprintf(out, "mylang_release(%s); ", obj->ast_temp_name);
                }
            }
        }
        AstNode* a = args;
        while (a) {
            if (call_needs_guard(a)) {
                if (is_retain) {
                    if (guard_needs_retain(a)) fprintf(out, "mylang_retain(%s); ", a->ast_temp_name);
                } else {
                    fprintf(out, "mylang_release(%s); ", a->ast_temp_name);
                }
            }
            a = a->next;
        }
    }
    int i;
    for (i = 0; i < expr->ast_child_count; i++) emit_call_guards(ctx, expr->ast_children[i], out, is_retain);
}

static void emit_stmt_call_retains(CodegenContext* ctx, AstNode* expr, FILE* out, int indent) {
    indent_line(out, indent);
    emit_call_guards(ctx, expr, out, 1);
    fprintf(out, "\n");
}
static void emit_stmt_call_releases(CodegenContext* ctx, AstNode* expr, FILE* out, int indent) {
    indent_line(out, indent);
    emit_call_guards(ctx, expr, out, 0);
    fprintf(out, "\n");
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
        ctx->cleanup_count++;
    }
}

static void cleanup_emit(CodegenContext* ctx, FILE* out, int indent) {
    int i;
    for (i = ctx->cleanup_count - 1; i >= 0; i--) {
        const char* name = ctx->cleanup_entries[i].name;
        indent_line(out, indent);
        if (ctx->cleanup_entries[i].is_weak) {
            fprintf(out, "mylang_weak_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface) {
            fprintf(out, "mylang_weak_release(%s.wr);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].weak_interface_array_size; j++) {
                fprintf(out, "mylang_weak_release(%s[%d].wr);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].interface_array_size; j++) {
                fprintf(out, "mylang_release(%s[%d].data);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface) {
            fprintf(out, "mylang_release(%s.data);\n", name);
        } else if (ctx->cleanup_entries[i].is_array) {
            fprintf(out, "mylang_array_free(&%s, %s, %d);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name,
                    ctx->cleanup_entries[i].array_elem_size_expr,
                    ctx->cleanup_entries[i].array_elem_kind);
        } else {
            fprintf(out, "mylang_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        }
    }
}

static void cleanup_push_scope(CodegenContext* ctx) {
    if (ctx->cleanup_scope_depth < MAX_SCOPE) {
        ctx->cleanup_scope_stack[ctx->cleanup_scope_depth++] = ctx->cleanup_count;
    }
}

static void cleanup_pop_scope(CodegenContext* ctx, FILE* out, int indent) {
    if (ctx->cleanup_scope_depth == 0) return;
    int saved = ctx->cleanup_scope_stack[--ctx->cleanup_scope_depth];
    int i;
    for (i = ctx->cleanup_count - 1; i >= saved; i--) {
        const char* name = ctx->cleanup_entries[i].name;
        indent_line(out, indent);
        if (ctx->cleanup_entries[i].is_weak) {
            fprintf(out, "mylang_weak_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface) {
            fprintf(out, "mylang_weak_release(%s.wr);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        } else if (ctx->cleanup_entries[i].is_weak_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].weak_interface_array_size; j++) {
                fprintf(out, "mylang_weak_release(%s[%d].wr);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface_array) {
            int j;
            for (j = 0; j < ctx->cleanup_entries[i].interface_array_size; j++) {
                fprintf(out, "mylang_release(%s[%d].data);\n", name, j);
            }
        } else if (ctx->cleanup_entries[i].is_interface) {
            fprintf(out, "mylang_release(%s.data);\n", name);
        } else if (ctx->cleanup_entries[i].is_array) {
            fprintf(out, "mylang_array_free(&%s, %s, %d);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name,
                    ctx->cleanup_entries[i].array_elem_size_expr,
                    ctx->cleanup_entries[i].array_elem_kind);
        } else {
            fprintf(out, "mylang_release(%s);\n",
                    strcmp(name, "this") == 0 ? "thiz" : name);
        }
    }
    ctx->cleanup_count = saved;
}

static void codegen_stmt(CodegenContext* ctx, AstNode* node, FILE* out, int indent);

static void indent_line(FILE* out, int indent) {
    int i;
    for (i = 0; i < indent; i++) fprintf(out, "    ");
}

static void codegen_body(CodegenContext* ctx, AstNode* body, FILE* out, int indent) {
    indent_line(out, indent);
    fprintf(out, "{\n");
    cleanup_push_scope(ctx);
    if (body->ast_kind == AST_BLOCK) {
        AstNode* s = body->ast_children[0];
        while (s) {
            codegen_stmt(ctx, s, out, indent + 1);
            s = s->next;
        }
    } else {
        codegen_stmt(ctx, body, out, indent + 1);
    }
    cleanup_pop_scope(ctx, out, indent + 1);
    indent_line(out, indent);
    fprintf(out, "}\n");
}

static void codegen_var_decl(CodegenContext* ctx, AstNode* node, FILE* out, int indent) {
    Type type = node->ast_resolved_type;

    if (node->ast_child_count > 0) {
        emit_subexpr_temps(ctx, node->ast_children[0], out, indent);
    }

    symtab_insert(node->ast_token.text, type);

    if (type.is_array) {
        char typename_buf[128];
        c_type_str(&type, typename_buf, sizeof(typename_buf));
        indent_line(out, indent);
        fprintf(out, "%s %s", typename_buf, node->ast_token.text);
        if (node->ast_child_count > 0) {
            fprintf(out, " = ");
            codegen_expr(ctx, node->ast_children[0], out);
        } else {
            fprintf(out, " = {0}");
        }
        fprintf(out, ";\n");
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
            int rhs_owned = (init->ast_kind == AST_CALL || init->ast_kind == AST_NEW);

            indent_line(out, indent);
            fprintf(out, "%s %s;\n", winame, node->ast_token.text);

            if (rhs_type.is_weak && rhs_type.type_kind == TYPE_INTERFACE) {
                /* weak-to-weak copy */
                indent_line(out, indent);
                fprintf(out, "%s.wr = mylang_weak_copy(", node->ast_token.text);
                codegen_expr(ctx, init, out);
                fprintf(out, ".wr);\n");
                indent_line(out, indent);
                fprintf(out, "%s.vt = ", node->ast_token.text);
                codegen_expr(ctx, init, out);
                fprintf(out, ".vt;\n");
            } else if (rhs_type.type_kind == TYPE_INTERFACE) {
                /* strong interface -> weak interface */
                if (rhs_owned) {
                    int tmp_id = ctx->assign_tmp_id++;
                    indent_line(out, indent);
                    fprintf(out, "%s _winit%d = ", type.class_name, tmp_id);
                    codegen_expr(ctx, init, out);
                    fprintf(out, ";\n");
                    indent_line(out, indent);
                    fprintf(out, "%s.wr = mylang_weak_init(_winit%d.data);\n",
                            node->ast_token.text, tmp_id);
                    indent_line(out, indent);
                    fprintf(out, "%s.vt = _winit%d.vtable;\n",
                            node->ast_token.text, tmp_id);
                    indent_line(out, indent);
                    fprintf(out, "mylang_release(_winit%d.data);\n", tmp_id);
                } else {
                    indent_line(out, indent);
                    fprintf(out, "%s.wr = mylang_weak_init(", node->ast_token.text);
                    codegen_expr(ctx, init, out);
                    fprintf(out, ".data);\n");
                    indent_line(out, indent);
                    fprintf(out, "%s.vt = ", node->ast_token.text);
                    codegen_expr(ctx, init, out);
                    fprintf(out, ".vtable;\n");
                }
            } else if (rhs_type.type_kind == TYPE_CLASS) {
                /* class -> weak interface */
                if (rhs_owned) {
                    int tmp_id = ctx->assign_tmp_id++;
                    indent_line(out, indent);
                    fprintf(out, "void* _winit%d = ", tmp_id);
                    codegen_expr(ctx, init, out);
                    fprintf(out, ";\n");
                    indent_line(out, indent);
                    fprintf(out, "%s.wr = mylang_weak_init(_winit%d);\n",
                            node->ast_token.text, tmp_id);
                    indent_line(out, indent);
                    fprintf(out, "%s.vt = &%s_%s_vtable;\n",
                            node->ast_token.text,
                            rhs_type.class_name,
                            type.class_name);
                    indent_line(out, indent);
                    fprintf(out, "mylang_release(_winit%d);\n", tmp_id);
                } else {
                    indent_line(out, indent);
                    fprintf(out, "%s.wr = mylang_weak_init(", node->ast_token.text);
                    codegen_expr(ctx, init, out);
                    fprintf(out, ");\n");
                    indent_line(out, indent);
                    fprintf(out, "%s.vt = &%s_%s_vtable;\n",
                            node->ast_token.text,
                            rhs_type.class_name,
                            type.class_name);
                }
            } else {
                fprintf(stderr, "error at %d:%d: cannot initialize weak interface '%s' with this value\n",
                        node->ast_token.line, node->ast_token.col, type.class_name);
                ctx->codegen_error = 1;
                indent_line(out, indent);
                fprintf(out, "%s %s = { NULL, NULL };\n", winame, node->ast_token.text);
            }
        } else {
            indent_line(out, indent);
            fprintf(out, "%s %s = { NULL, NULL };\n", winame, node->ast_token.text);
        }
        cleanup_add_weak_interface(ctx, node->ast_token.text);
        return;
    }

    if (type.is_weak && node->ast_child_count > 0) {
        resolve_type(node->ast_children[0]);
        if (node->ast_children[0]->ast_resolved_type.is_weak) {
            indent_line(out, indent);
            fprintf(out, "WeakRef* %s = mylang_weak_copy(", node->ast_token.text);
            codegen_expr(ctx, node->ast_children[0], out);
            fprintf(out, ");\n");
        } else {
            int rhs_owned = (node->ast_children[0]->ast_kind == AST_CALL ||
                             node->ast_children[0]->ast_kind == AST_NEW);
            if (rhs_owned) {
                int tmp_id = ctx->assign_tmp_id++;
                indent_line(out, indent);
                fprintf(out, "void* _w%d = ", tmp_id);
                codegen_expr(ctx, node->ast_children[0], out);
                fprintf(out, ";\n");
                indent_line(out, indent);
                fprintf(out, "WeakRef* %s = mylang_weak_init(_w%d);\n",
                        node->ast_token.text, tmp_id);
                indent_line(out, indent);
                fprintf(out, "mylang_release(_w%d);\n", tmp_id);
            } else {
                indent_line(out, indent);
                fprintf(out, "WeakRef* %s = mylang_weak_init(", node->ast_token.text);
                codegen_expr(ctx, node->ast_children[0], out);
                fprintf(out, ");\n");
            }
        }
        cleanup_add(ctx, node->ast_token.text, 1, 0);
        return;
    }

    if (type.type_kind == TYPE_INTERFACE) {
        if (node->ast_child_count > 0) {
            AstNode* init = node->ast_children[0];
            resolve_type(init);
            int rhs_owned = (init->ast_kind == AST_CALL || init->ast_kind == AST_NEW);

            if (init->ast_resolved_type.type_kind == TYPE_CLASS) {
                indent_line(out, indent);
                fprintf(out, "%s %s;\n", type.class_name, node->ast_token.text);
                indent_line(out, indent);
                fprintf(out, "%s.data = (void*)", node->ast_token.text);
                if (!rhs_owned) fprintf(out, "mylang_retain(");
                codegen_expr(ctx, init, out);
                if (!rhs_owned) fprintf(out, ")");
                fprintf(out, ";\n");
                indent_line(out, indent);
                fprintf(out, "%s.vtable = &%s_%s_vtable;\n",
                        node->ast_token.text,
                        init->ast_resolved_type.class_name,
                        type.class_name);
            } else if (init->ast_resolved_type.type_kind == TYPE_INTERFACE) {
                int tmp_id = ctx->assign_tmp_id++;
                indent_line(out, indent);
                fprintf(out, "%s _init%d = ", type.class_name, tmp_id);
                codegen_expr(ctx, init, out);
                fprintf(out, ";\n");
                if (!rhs_owned) {
                    indent_line(out, indent);
                    fprintf(out, "mylang_retain(_init%d.data);\n", tmp_id);
                }
                indent_line(out, indent);
                fprintf(out, "%s %s = _init%d;\n", type.class_name, node->ast_token.text, tmp_id);
            } else {
                fprintf(stderr, "error at %d:%d: cannot initialize interface '%s' with non-class value\n",
                        node->ast_token.line, node->ast_token.col, type.class_name);
                ctx->codegen_error = 1;
                indent_line(out, indent);
                fprintf(out, "%s %s = { NULL, NULL };\n", type.class_name, node->ast_token.text);
            }
        } else {
            indent_line(out, indent);
            fprintf(out, "%s %s = { NULL, NULL };\n", type.class_name, node->ast_token.text);
        }
        cleanup_add(ctx, node->ast_token.text, 0, 1);
        return;
    }

    indent_line(out, indent);

    {
        char typename_buf[128];
        c_type_str(&type, typename_buf, sizeof(typename_buf));
        fprintf(out, "%s %s", typename_buf, node->ast_token.text);
    }

    if (node->ast_child_count > 0) {
        if (type.type_kind == TYPE_CLASS && node->ast_children[0]->ast_kind != AST_NEW
            && node->ast_children[0]->ast_kind != AST_CALL) {
            fprintf(out, " = mylang_retain(");
            codegen_expr(ctx, node->ast_children[0], out);
            fprintf(out, ")");
        } else {
            fprintf(out, " = ");
            codegen_expr(ctx, node->ast_children[0], out);
        }
    } else if (type.is_pointer || type.is_weak) {
        fprintf(out, " = NULL");
    }
    fprintf(out, ";\n");

    if (type.type_kind == TYPE_CLASS && type.is_pointer) {
        cleanup_add(ctx, node->ast_token.text, 0, 0);
    }
    if (type.is_weak) {
        cleanup_add(ctx, node->ast_token.text, 1, 0);
    }
}

static void codegen_if_stmt(CodegenContext* ctx, AstNode* node, FILE* out, int indent) {
    emit_subexpr_temps(ctx, node->ast_children[0], out, indent);
    emit_bounds_checks(ctx, node->ast_children[0], out, indent);
    indent_line(out, indent);
    fprintf(out, "if (");
    codegen_expr(ctx, node->ast_children[0], out);
    fprintf(out, ")\n");
    codegen_body(ctx, node->ast_children[1], out, indent);

    if (node->ast_child_count > 2) {
        indent_line(out, indent);
        fprintf(out, "else\n");
        codegen_body(ctx, node->ast_children[2], out, indent);
    }
}

static void codegen_while_stmt(CodegenContext* ctx, AstNode* node, FILE* out, int indent) {
    emit_subexpr_temps(ctx, node->ast_children[0], out, indent);
    emit_bounds_checks(ctx, node->ast_children[0], out, indent);
    indent_line(out, indent);
    fprintf(out, "while (");
    codegen_expr(ctx, node->ast_children[0], out);
    fprintf(out, ")\n");
    codegen_body(ctx, node->ast_children[1], out, indent);
}

static void codegen_return_stmt(CodegenContext* ctx, AstNode* node, FILE* out, int indent) {
    if (node->ast_child_count > 0) {
        AstNode* ret = node->ast_children[0];
        emit_subexpr_temps(ctx, ret, out, indent);
        emit_bounds_checks(ctx, ret, out, indent);
        resolve_type(ret);

        if (ret->ast_resolved_type.type_kind == TYPE_CLASS &&
            ctx->return_type.type_kind == TYPE_INTERFACE) {
            /* implicit class-to-interface conversion in return */
            int needs_retain = return_expr_needs_retain(ret);
            int tid = ctx->assign_tmp_id++;
            indent_line(out, indent);
            if (needs_retain) {
                fprintf(out, "void* _r = mylang_retain(");
            } else {
                fprintf(out, "void* _r = (");
            }
            codegen_expr(ctx, ret, out);
            fprintf(out, ");\n");
            indent_line(out, indent);
            fprintf(out, "%s _iret%d;\n", ctx->return_type.class_name, tid);
            indent_line(out, indent);
            fprintf(out, "_iret%d.data = _r;\n", tid);
            indent_line(out, indent);
            fprintf(out, "_iret%d.vtable = &%s_%s_vtable;\n",
                    tid, ret->ast_resolved_type.class_name, ctx->return_type.class_name);
            cleanup_emit(ctx, out, indent);
            indent_line(out, indent);
            fprintf(out, "MY_POP();\n");
            indent_line(out, indent);
            fprintf(out, "return _iret%d;\n", tid);
        } else if (ret->ast_resolved_type.type_kind == TYPE_CLASS) {
            int needs_retain = return_expr_needs_retain(ret);
            indent_line(out, indent);
            if (needs_retain) {
                fprintf(out, "void* _r = mylang_retain(");
            } else {
                fprintf(out, "void* _r = (");
            }
            codegen_expr(ctx, ret, out);
            fprintf(out, ");\n");
            cleanup_emit(ctx, out, indent);
            indent_line(out, indent);
            fprintf(out, "MY_POP();\n");
            indent_line(out, indent);
            fprintf(out, "return _r;\n");
        } else if (ret->ast_resolved_type.is_array) {
            fprintf(stderr, "error at %d:%d: cannot return array by value; use move_to(ref) through a ref parameter\n",
                    ret->ast_token.line, ret->ast_token.col);
            ctx->codegen_error = 1;
            indent_line(out, indent);
            fprintf(out, "/* invalid array return */\n");
            cleanup_emit(ctx, out, indent);
            indent_line(out, indent);
            fprintf(out, "MY_POP();\n");
            indent_line(out, indent);
            fprintf(out, "return;\n");
        } else if (ret->ast_resolved_type.type_kind == TYPE_INTERFACE) {
            int needs_retain = return_expr_needs_retain(ret);
            char tbuf[128];
            c_type_str(&ret->ast_resolved_type, tbuf, sizeof(tbuf));
            int tid = ctx->assign_tmp_id++;
            indent_line(out, indent);
            fprintf(out, "%s _iret%d = ", tbuf, tid);
            codegen_expr(ctx, ret, out);
            fprintf(out, ";\n");
            if (needs_retain) {
                indent_line(out, indent);
                fprintf(out, "mylang_retain(_iret%d.data);\n", tid);
            }
            cleanup_emit(ctx, out, indent);
            indent_line(out, indent);
            fprintf(out, "MY_POP();\n");
            indent_line(out, indent);
            fprintf(out, "return _iret%d;\n", tid);
        } else {
            char tbuf[128];
            c_type_str(&ret->ast_resolved_type, tbuf, sizeof(tbuf));
            indent_line(out, indent);
            fprintf(out, "%s _mylang_ret = ", tbuf);
            codegen_expr(ctx, ret, out);
            fprintf(out, ";\n");
            cleanup_emit(ctx, out, indent);
            indent_line(out, indent);
            fprintf(out, "MY_POP();\n");
            indent_line(out, indent);
            fprintf(out, "return _mylang_ret;\n");
        }
    } else {
        cleanup_emit(ctx, out, indent);
        indent_line(out, indent);
        fprintf(out, "MY_POP();\n");
        indent_line(out, indent);
        fprintf(out, "return;\n");
    }
}

static void codegen_expr_stmt(CodegenContext* ctx, AstNode* node, FILE* out, int indent) {
    emit_bounds_checks(ctx, node->ast_children[0], out, indent);
    AstNode* expr = node->ast_children[0];
    resolve_type(expr);

    /* Extract owned class/interface subexpressions (e.g. inside 'as' casts or
       comparisons) into temporaries so they are evaluated once and released. */
    emit_subexpr_temps(ctx, expr, out, indent);

    /* Extract guarded class subexpressions into temporaries so side-effecting
       arguments (e.g. method calls) are evaluated exactly once. */
    emit_guarded_temp_decls(ctx, expr, out, indent);

    /* class/array assignment: evaluate RHS first, release old LHS, then assign.
       This avoids use-after-free when RHS aliases LHS (e.g. b = b.set(5)). */
    if (expr->ast_kind == AST_ASSIGN) {
        AstNode* lhs = expr->ast_children[0];
        AstNode* rhs = expr->ast_children[1];
        resolve_type(lhs);
        resolve_type(rhs);
        Type lt = lhs->ast_resolved_type;

        if (lhs->ast_kind == AST_ARRAY_ACCESS) {
            Type at = lhs->ast_children[0]->ast_resolved_type;
            if (at.is_array) {
                emit_stmt_call_retains(ctx, expr, out, indent);
                indent_line(out, indent);
                codegen_expr(ctx, expr, out);
                fprintf(out, ";\n");
                emit_stmt_call_releases(ctx, expr, out, indent);
                return;
            }
        }

        if (lhs->ast_kind == AST_IDENT && lt.type_kind == TYPE_CLASS) {
            emit_stmt_call_retains(ctx, expr, out, indent);

            int id = ctx->assign_tmp_id++;
            int rhs_owned = (rhs->ast_kind == AST_CALL || rhs->ast_kind == AST_NEW);

            indent_line(out, indent);
            fprintf(out, "void* _my_assign_%d = ", id);
            if (!rhs_owned) {
                fprintf(out, "mylang_retain(");
                codegen_expr(ctx, rhs, out);
                fprintf(out, ")");
            } else {
                codegen_expr(ctx, rhs, out);
            }
            fprintf(out, ";\n");

            indent_line(out, indent);
            fprintf(out, "mylang_release(");
            codegen_expr(ctx, lhs, out);
            fprintf(out, ");\n");

            indent_line(out, indent);
            codegen_expr(ctx, lhs, out);
            fprintf(out, " = _my_assign_%d;\n", id);

            emit_stmt_call_releases(ctx, expr, out, indent);
            return;
        }

        if (lhs->ast_kind == AST_IDENT && lt.is_weak && lt.type_kind == TYPE_INTERFACE) {
            emit_stmt_call_retains(ctx, expr, out, indent);
            resolve_type(rhs);
            Type rt = rhs->ast_resolved_type;
            int rhs_owned = (rhs->ast_kind == AST_CALL || rhs->ast_kind == AST_NEW);

            /* release old weak ref first */
            indent_line(out, indent);
            fprintf(out, "mylang_weak_release(%s.wr);\n", lhs->ast_token.text);

            if (rt.is_weak && rt.type_kind == TYPE_INTERFACE) {
                indent_line(out, indent);
                fprintf(out, "%s.wr = mylang_weak_copy(", lhs->ast_token.text);
                codegen_expr(ctx, rhs, out);
                fprintf(out, ".wr);\n");
                indent_line(out, indent);
                fprintf(out, "%s.vt = ", lhs->ast_token.text);
                codegen_expr(ctx, rhs, out);
                fprintf(out, ".vt;\n");
            } else if (rt.type_kind == TYPE_INTERFACE) {
                if (rhs_owned) {
                    int id = ctx->assign_tmp_id++;
                    indent_line(out, indent);
                    fprintf(out, "%s _wassign%d = ", lt.class_name, id);
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ";\n");
                    indent_line(out, indent);
                    fprintf(out, "%s.wr = mylang_weak_init(_wassign%d.data);\n",
                            lhs->ast_token.text, id);
                    indent_line(out, indent);
                    fprintf(out, "%s.vt = _wassign%d.vtable;\n",
                            lhs->ast_token.text, id);
                    indent_line(out, indent);
                    fprintf(out, "mylang_release(_wassign%d.data);\n", id);
                } else {
                    indent_line(out, indent);
                    fprintf(out, "%s.wr = mylang_weak_init(", lhs->ast_token.text);
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ".data);\n");
                    indent_line(out, indent);
                    fprintf(out, "%s.vt = ", lhs->ast_token.text);
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ".vtable;\n");
                }
            } else if (rt.type_kind == TYPE_CLASS) {
                if (rhs_owned) {
                    int id = ctx->assign_tmp_id++;
                    indent_line(out, indent);
                    fprintf(out, "void* _wassign%d = ", id);
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ";\n");
                    indent_line(out, indent);
                    fprintf(out, "%s.wr = mylang_weak_init(_wassign%d);\n",
                            lhs->ast_token.text, id);
                    indent_line(out, indent);
                    fprintf(out, "%s.vt = &%s_%s_vtable;\n",
                            lhs->ast_token.text, rt.class_name, lt.class_name);
                    indent_line(out, indent);
                    fprintf(out, "mylang_release(_wassign%d);\n", id);
                } else {
                    indent_line(out, indent);
                    fprintf(out, "%s.wr = mylang_weak_init(", lhs->ast_token.text);
                    codegen_expr(ctx, rhs, out);
                    fprintf(out, ");\n");
                    indent_line(out, indent);
                    fprintf(out, "%s.vt = &%s_%s_vtable;\n",
                            lhs->ast_token.text, rt.class_name, lt.class_name);
                }
            } else {
                fprintf(stderr, "error at %d:%d: cannot assign to weak interface '%s' from this type\n",
                        rhs->ast_token.line, rhs->ast_token.col, lt.class_name);
                ctx->codegen_error = 1;
            }

            emit_stmt_call_releases(ctx, expr, out, indent);
            return;
        }

        if (lhs->ast_kind == AST_IDENT && lt.type_kind == TYPE_INTERFACE) {
            emit_stmt_call_retains(ctx, expr, out, indent);
            resolve_type(rhs);
            Type rt = rhs->ast_resolved_type;

            int id = ctx->assign_tmp_id++;
            int rhs_owned = (rhs->ast_kind == AST_CALL || rhs->ast_kind == AST_NEW);

            if (rt.type_kind == TYPE_CLASS) {
                indent_line(out, indent);
                fprintf(out, "void* _iassign_%d = ", id);
                if (!rhs_owned) fprintf(out, "mylang_retain(");
                codegen_expr(ctx, rhs, out);
                if (!rhs_owned) fprintf(out, ")");
                fprintf(out, ";\n");

                indent_line(out, indent);
                fprintf(out, "mylang_release(%s.data);\n", lhs->ast_token.text);
                indent_line(out, indent);
                fprintf(out, "%s.data = _iassign_%d;\n", lhs->ast_token.text, id);
                indent_line(out, indent);
                fprintf(out, "%s.vtable = &%s_%s_vtable;\n",
                        lhs->ast_token.text, rt.class_name, lt.class_name);
            } else if (rt.type_kind == TYPE_INTERFACE) {
                indent_line(out, indent);
                fprintf(out, "%s _iassign_%d = ", c_base_name(&lt), id);
                codegen_expr(ctx, rhs, out);
                fprintf(out, ";\n");

                if (!rhs_owned) {
                    indent_line(out, indent);
                    fprintf(out, "mylang_retain(_iassign_%d.data);\n", id);
                }

                indent_line(out, indent);
                fprintf(out, "mylang_release(%s.data);\n", lhs->ast_token.text);
                indent_line(out, indent);
                codegen_expr(ctx, lhs, out);
                fprintf(out, " = _iassign_%d;\n", id);
            } else {
                fprintf(stderr, "error at %d:%d: cannot assign non-class value to interface '%s'\n",
                        rhs->ast_token.line, rhs->ast_token.col, lt.class_name);
                ctx->codegen_error = 1;
            }

            emit_stmt_call_releases(ctx, expr, out, indent);
            return;
        }
    }

    emit_stmt_call_retains(ctx, expr, out, indent);

    indent_line(out, indent);
    if (expr->ast_kind == AST_CALL && expr->ast_resolved_type.type_kind == TYPE_CLASS) {
        /* discarded class return: release the +1 from callee */
        fprintf(out, "(void)mylang_release(");
        codegen_expr(ctx, expr, out);
        fprintf(out, ")");
    } else if (expr->ast_kind == AST_CALL && expr->ast_resolved_type.type_kind == TYPE_INTERFACE) {
        /* discarded interface return: save to temp, release .data */
        int dtid = ctx->assign_tmp_id++;
        fprintf(out, "%s _dt%d = ", c_base_name(&expr->ast_resolved_type), dtid);
        codegen_expr(ctx, expr, out);
        fprintf(out, "; (void)mylang_release(_dt%d.data)", dtid);
    } else {
        codegen_expr(ctx, expr, out);
    }
    fprintf(out, ";\n");

    emit_stmt_call_releases(ctx, expr, out, indent);
}

static void codegen_stmt(CodegenContext* ctx, AstNode* node, FILE* out, int indent) {
    if (!node) return;

    switch (node->ast_kind) {
        case AST_BLOCK:
            codegen_body(ctx, node, out, indent);
            break;
        case AST_VAR_DECL:
            codegen_var_decl(ctx, node, out, indent);
            break;
        case AST_IF_STMT:
            codegen_if_stmt(ctx, node, out, indent);
            break;
        case AST_WHILE_STMT:
            codegen_while_stmt(ctx, node, out, indent);
            break;
        case AST_RETURN_STMT:
            codegen_return_stmt(ctx, node, out, indent);
            break;
        case AST_EXPR_STMT:
            codegen_expr_stmt(ctx, node, out, indent);
            break;
        default:
            indent_line(out, indent);
            fprintf(out, "/* unknown stmt kind=%d */\n", node->ast_kind);
            break;
    }
}


static void codegen_method_decl(CodegenContext* ctx, AstNode* node, FILE* out, const char* class_name);

static void codegen_interface_typedefs(CodegenContext* ctx, FILE* out) {
    extern InterfaceInfo* interface_list;
    InterfaceInfo* ii = interface_list;
    while (ii) {
        /* VTable typedef */
        fprintf(out, "typedef struct %sVTable {\n", ii->name);
        fprintf(out, "    uint32_t concrete_type_id;\n");
        int j;
        for (j = 0; j < ii->method_count; j++) {
            InterfaceMethodInfo* im = &ii->methods[j];
            char rbuf[128];
            c_type_str(&im->return_type, rbuf, sizeof(rbuf));
            fprintf(out, "    %s (*%s)(void* thiz", rbuf, im->name);
            int k;
            for (k = 0; k < im->param_count; k++) {
                char pbuf[128];
                c_type_str(&im->param_types[k], pbuf, sizeof(pbuf));
                if (im->param_types[k].is_ref) {
                    fprintf(out, ", %s*", pbuf);
                } else {
                    fprintf(out, ", %s", pbuf);
                }
            }
            fprintf(out, ");\n");
        }
        fprintf(out, "} %sVTable;\n\n", ii->name);

        /* Fat pointer typedef (tagged so it can be forward-declared) */
        fprintf(out, "typedef struct %s {\n", ii->name);
        fprintf(out, "    void* data;\n");
        fprintf(out, "    const %sVTable* vtable;\n", ii->name);
        fprintf(out, "} %s;\n\n", ii->name);

        /* Weak fat pointer typedef */
        fprintf(out, "typedef struct Weak%s {\n", ii->name);
        fprintf(out, "    WeakRef* wr;\n");
        fprintf(out, "    %sVTable* vt;\n", ii->name);
        fprintf(out, "} Weak%s;\n\n", ii->name);

        /* Weak interface lock helper returns a strong fat pointer */
        fprintf(out, "static %s mylang_lock_%s(WeakRef* wr, %sVTable* vt) {\n",
                ii->name, ii->name, ii->name);
        fprintf(out, "    void* _p = mylang_lock(wr);\n");
        fprintf(out, "    %s _r;\n", ii->name);
        fprintf(out, "    _r.data = _p;\n");
        fprintf(out, "    _r.vtable = vt;\n");
        fprintf(out, "    return _r;\n");
        fprintf(out, "}\n\n");

        /* Weak interface conversion helpers */
        fprintf(out, "static Weak%s mylang_weakify_%s(%s s) {\n",
                ii->name, ii->name, ii->name);
        fprintf(out, "    Weak%s w;\n", ii->name);
        fprintf(out, "    w.wr = mylang_weak_init(s.data);\n");
        fprintf(out, "    w.vt = s.vtable;\n");
        fprintf(out, "    return w;\n");
        fprintf(out, "}\n\n");

        fprintf(out, "static Weak%s mylang_weakify_%s_owned(%s s) {\n",
                ii->name, ii->name, ii->name);
        fprintf(out, "    Weak%s w;\n", ii->name);
        fprintf(out, "    w.wr = mylang_weak_init(s.data);\n");
        fprintf(out, "    w.vt = s.vtable;\n");
        fprintf(out, "    mylang_release(s.data);\n");
        fprintf(out, "    return w;\n");
        fprintf(out, "}\n\n");

        fprintf(out, "static Weak%s mylang_weakify_%s_from_ptr(void* p, %sVTable* vt) {\n",
                ii->name, ii->name, ii->name);
        fprintf(out, "    Weak%s w;\n", ii->name);
        fprintf(out, "    w.wr = mylang_weak_init(p);\n");
        fprintf(out, "    w.vt = vt;\n");
        fprintf(out, "    return w;\n");
        fprintf(out, "}\n\n");

        fprintf(out, "static Weak%s mylang_weakify_%s_from_ptr_owned(void* p, %sVTable* vt) {\n",
                ii->name, ii->name, ii->name);
        fprintf(out, "    Weak%s w;\n", ii->name);
        fprintf(out, "    w.wr = mylang_weak_init(p);\n");
        fprintf(out, "    w.vt = vt;\n");
        fprintf(out, "    mylang_release(p);\n");
        fprintf(out, "    return w;\n");
        fprintf(out, "}\n\n");
        ii = ii->next;
    }
}

static void codegen_class_interface_vtables(CodegenContext* ctx, ClassInfo* ci, FILE* out) {
    const char* class_c = class_c_name(ci);
    int i;
    for (i = 0; i < ci->impl_count; i++) {
        const char* iface_name = ci->impl_names[i];
        InterfaceInfo* ii = symtab_find_interface(iface_name);
        if (!ii) continue;

        /* emit thunk functions */
        int j;
        for (j = 0; j < ii->method_count; j++) {
            InterfaceMethodInfo* im = &ii->methods[j];
            char rbuf[128];
            c_type_str(&im->return_type, rbuf, sizeof(rbuf));

            /* static RetType Class_IFace_method(void* thiz, params...) */
            fprintf(out, "static %s %s_%s_%s(void* _p", rbuf, class_c, iface_name, im->name);
            int k;
            for (k = 0; k < im->param_count; k++) {
                char pbuf[128];
                c_type_str(&im->param_types[k], pbuf, sizeof(pbuf));
                if (im->param_types[k].is_ref) {
                    fprintf(out, ", %s* _a%d", pbuf, k);
                } else {
                    fprintf(out, ", %s _a%d", pbuf, k);
                }
            }
            fprintf(out, ") {\n");

            /* call actual method */
            fprintf(out, "    ");
            if (im->return_type.type_kind != TYPE_VOID) {
                fprintf(out, "return ");
            }
            fprintf(out, "%s_%s((%s*)_p", class_c, im->name, class_c);
            for (k = 0; k < im->param_count; k++) {
                fprintf(out, ", _a%d", k);
            }
            fprintf(out, ");\n");
            fprintf(out, "}\n\n");
        }

        /* emit static vtable */
        fprintf(out, "static const %sVTable %s_%s_vtable = {\n", iface_name, class_c, iface_name);
        fprintf(out, "    .concrete_type_id = %u,\n", (unsigned)ci->type_id);
        for (j = 0; j < ii->method_count; j++) {
            fprintf(out, "    .%s = %s_%s_%s", ii->methods[j].name, class_c, iface_name, ii->methods[j].name);
            if (j < ii->method_count - 1) fprintf(out, ",");
            fprintf(out, "\n");
        }
        fprintf(out, "};\n\n");
    }
}

static void codegen_struct_decl(CodegenContext* ctx, AstNode* node, FILE* out) {
    StructInfo* si = symtab_find_struct(node->ast_token.text);
    if (!si) return;

    fprintf(out, "typedef struct %s {\n", node->ast_token.text);
    int i;
    if (si->field_count == 0) {
        fprintf(out, "    char _pad;\n");
    } else {
        for (i = 0; i < si->field_count; i++) {
            char ftype_buf[128];
            c_type_str(&si->field_types[i], ftype_buf, sizeof(ftype_buf));
            fprintf(out, "    %s %s;\n", ftype_buf, si->field_names[i]);
        }
    }
    fprintf(out, "} %s;\n\n", node->ast_token.text);
}

/* Emit the per-class finalizer that releases reference-counted fields before
   the object's memory is freed by mylang_release. */
static void codegen_class_destructor(CodegenContext* ctx, ClassInfo* ci, const char* class_c, FILE* out) {
    (void)ctx;
    fprintf(out, "static void _mylang_dtor_%s(%s* p) {\n", class_c, class_c);
    int i;
    for (i = 0; i < ci->field_count; i++) {
        Type ft = ci->field_types[i];
        const char* fname = ci->field_names[i];
        if (ft.is_array) {
            char esz[64];
            array_elem_size_expr(&ft, esz, sizeof(esz));
            fprintf(out, "    mylang_array_free(&p->%s, %s, %d);\n",
                    fname, esz, array_elem_kind(&ft));
        } else if (ft.is_weak && ft.type_kind == TYPE_INTERFACE) {
            fprintf(out, "    mylang_weak_release(p->%s.wr);\n", fname);
        } else if (ft.is_weak) {
            fprintf(out, "    mylang_weak_release(p->%s);\n", fname);
        } else if (ft.type_kind == TYPE_INTERFACE) {
            fprintf(out, "    mylang_release(p->%s.data);\n", fname);
        } else if (ft.type_kind == TYPE_CLASS) {
            fprintf(out, "    mylang_release(p->%s);\n", fname);
        }
    }
    fprintf(out, "}\n\n");
}

static void codegen_class_decl(CodegenContext* ctx, AstNode* node, FILE* out) {
    ClassInfo* ci = symtab_find_class_by_mangled(node->ast_token.text);
    if (!ci) ci = symtab_find_class(node->ast_token.text);
    if (!ci) return;
    if (ci->is_generic) return;

    const char* class_c = class_c_name(ci);

    fprintf(out, "typedef struct %s {\n", class_c);
    int i;
    if (ci->field_count == 0) {
        fprintf(out, "    char _pad;\n");
    } else {
        for (i = 0; i < ci->field_count; i++) {
            char ftype_buf[128];
            if (ci->field_types[i].type_kind == TYPE_CLASS &&
                ci->field_types[i].type_arg_count == 0 &&
                strcmp(ci->field_types[i].class_name, node->ast_token.text) == 0) {
                int n = snprintf(ftype_buf, sizeof(ftype_buf), "struct %s*", ci->field_types[i].class_name);
                CHECK_SNPRINTF(n, (size_t)sizeof(ftype_buf), "field type name too long");
            } else {
                c_type_str(&ci->field_types[i], ftype_buf, sizeof(ftype_buf));
            }
            fprintf(out, "    %s %s;\n", ftype_buf, ci->field_names[i]);
        }
    }
    fprintf(out, "} %s;\n\n", class_c);

    codegen_class_destructor(ctx, ci, class_c, out);

    /* emit method declarations */
    AstNode* m = node->ast_children[0];
    while (m) {
        codegen_method_decl(ctx, m, out, class_c);
        m = m->next;
    }

    /* emit interface thunks and vtables */
    codegen_class_interface_vtables(ctx, ci, out);
}


static void codegen_method_decl(CodegenContext* ctx, AstNode* node, FILE* out, const char* class_name) {
    if (node->ast_resolved_type.is_array) {
        fprintf(stderr, "error at %d:%d: method '%s.%s' cannot return array by value\n",
                node->ast_token.line, node->ast_token.col, class_name, node->ast_token.text);
        ctx->codegen_error = 1;
    }
    char ret_buf[128];
    c_type_str(&node->ast_resolved_type, ret_buf, sizeof(ret_buf));
    fprintf(out, "%s %s_%s(%s* thiz", ret_buf, class_name, node->ast_token.text, class_name);
    AstNode* params = NULL; AstNode* body = NULL;
    if (node->ast_child_count == 2) { params = node->ast_children[0]; body = node->ast_children[1]; }
    else { body = node->ast_children[0]; }
    { AstNode* p = params; while (p) { fprintf(out, ", ");
        if (p->ast_resolved_type.is_array && !type_is_ref(&p->ast_resolved_type)) {
            fprintf(stderr, "error at %d:%d: array parameter '%s' must be ref\n",
                    p->ast_token.line, p->ast_token.col, p->ast_token.text);
            ctx->codegen_error = 1;
        }
        char pt[128]; c_type_str(&p->ast_resolved_type, pt, sizeof(pt));
        if (type_is_ref(&p->ast_resolved_type)) {
            fprintf(out, "%s* %s", pt, p->ast_token.text);
        } else {
            fprintf(out, "%s %s", pt, p->ast_token.text);
        }
        p = p->next; } }
    fprintf(out, ")\n{\n");
    cleanup_push_scope(ctx); symtab_enter_scope();
    Type prev_ret = ctx->return_type;
    ctx->return_type = node->ast_resolved_type;
    Type thiz_type; memset(&thiz_type, 0, sizeof(thiz_type));
    thiz_type.type_kind = TYPE_CLASS;
    CHECK_STRSCPY(strscpy(thiz_type.class_name, class_name, sizeof(thiz_type.class_name)), "class name too long");
    thiz_type.is_pointer = 1; symtab_insert("this", thiz_type);
    { AstNode* p = params; while (p) { symtab_insert(p->ast_token.text, p->ast_resolved_type);
        if (p->ast_resolved_type.is_weak && p->ast_resolved_type.type_kind == TYPE_INTERFACE) {
            cleanup_add_weak_interface(ctx, p->ast_token.text);
        } else if (p->ast_resolved_type.is_weak) {
            cleanup_add(ctx, p->ast_token.text, 1, 0);
        }
        p = p->next; } }

    indent_line(out, 1);
    fprintf(out, "MY_PUSH(\"%s.%s\", \"%s\", %d);\n", class_name, node->ast_token.text, ctx->source_file_escaped, node->ast_token.line);

    fprintf(out, "{\n");
    if (body && body->ast_kind == AST_BLOCK) { AstNode* s = body->ast_children[0];
        while (s) { codegen_stmt(ctx, s, out, 2); s = s->next; } }
    cleanup_pop_scope(ctx, out, 2);
    fprintf(out, "}\n");

    cleanup_pop_scope(ctx, out, 1);
    symtab_exit_scope();
    indent_line(out, 1);
    fprintf(out, "MY_POP();\n");
    fprintf(out, "}\n\n");
    ctx->return_type = prev_ret;
}
static void codegen_func_decl(CodegenContext* ctx, AstNode* node, FILE* out) {
    const char* func_name = node->ast_token.text;
    if (strcmp(func_name, "main") == 0) {
        func_name = "_my_main";
        ctx->has_main = 1;
        ctx->main_return_type = node->ast_resolved_type;
    }

    /* return type */
    if (node->ast_resolved_type.is_array) {
        fprintf(stderr, "error at %d:%d: function '%s' cannot return array by value\n",
                node->ast_token.line, node->ast_token.col, func_name);
        ctx->codegen_error = 1;
    }
    {
        char ret_buf[128];
        c_type_str(&node->ast_resolved_type, ret_buf, sizeof(ret_buf));
        fprintf(out, "%s %s(", ret_buf, func_name);
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
            if (!first) fprintf(out, ", ");
            if (p->ast_resolved_type.is_array && !type_is_ref(&p->ast_resolved_type)) {
                fprintf(stderr, "error at %d:%d: array parameter '%s' must be ref\n",
                        p->ast_token.line, p->ast_token.col, p->ast_token.text);
                ctx->codegen_error = 1;
            }
            char ptype_buf[128];
            c_type_str(&p->ast_resolved_type, ptype_buf, sizeof(ptype_buf));
            if (type_is_ref(&p->ast_resolved_type)) {
                fprintf(out, "%s* %s", ptype_buf, p->ast_token.text);
            } else {
                fprintf(out, "%s %s", ptype_buf, p->ast_token.text);
            }
            first = 0;
            p = p->next;
        }
    }
    fprintf(out, ")\n");

    /* body */
    cleanup_push_scope(ctx);
    fprintf(out, "{\n");

    symtab_enter_scope();

    Type prev_ret = ctx->return_type;
    ctx->return_type = node->ast_resolved_type;

    /* register parameters in scope */
    {
        AstNode* p = params;
        while (p) {
            symtab_insert(p->ast_token.text, p->ast_resolved_type);
            if (p->ast_resolved_type.is_weak && p->ast_resolved_type.type_kind == TYPE_INTERFACE) {
                cleanup_add_weak_interface(ctx, p->ast_token.text);
            } else if (p->ast_resolved_type.is_weak) {
                cleanup_add(ctx, p->ast_token.text, 1, 0);
            }
            p = p->next;
        }
    }

    indent_line(out, 1);
    fprintf(out, "MY_PUSH(\"%s\", \"%s\", %d);\n", func_name, ctx->source_file_escaped, node->ast_token.line);

    fprintf(out, "{\n");

    /* walk body statements */
    if (body && body->ast_kind == AST_BLOCK) {
        AstNode* s = body->ast_children[0];
        while (s) {
            codegen_stmt(ctx, s, out, 2);
            s = s->next;
        }
    }

    cleanup_pop_scope(ctx, out, 2);
    fprintf(out, "}\n");

    cleanup_pop_scope(ctx, out, 1);
    symtab_exit_scope();
    indent_line(out, 1);
    fprintf(out, "MY_POP();\n");
    fprintf(out, "}\n\n");
    ctx->return_type = prev_ret;
}

void codegen_program(AstNode* program, FILE* out, const char* source_file, int leak_check) {
    CodegenContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    s_last_codegen_error = 0;
    ctx.source_file = source_file ? source_file : "";
    escape_source_file(&ctx, ctx.source_file);

    if (symtab_validate_impls() != 0) {
        fprintf(stderr, "error: semantic errors found, no output generated\n");
        ctx.codegen_error = 1;
        s_last_codegen_error = 1;
        return;
    }

    preinstantiate_generic_types(program);

    fprintf(out, "/* Generated by MyLang compiler */\n");
    if (leak_check) {
        fprintf(out, "#define MYLANG_LEAK_CHECK\n");
    }
    fprintf(out, "#define _CRTDBG_MAP_ALLOC\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stddef.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#ifdef _MSC_VER\n");
    fprintf(out, "#include <intrin.h>\n");
    fprintf(out, "#include <crtdbg.h>\n");
    fprintf(out, "#else\n");
    fprintf(out, "#define __debugbreak() __builtin_trap()\n");
    fprintf(out, "#endif\n\n");

    fprintf(out, "#include \"runtime.h\"\n\n");
    /* Forward-declare class and interface types so that interface vtables
       can reference class/interface return/parameter types regardless of
       declaration order. */
    {
        ClassInfo* ci = class_list;
        while (ci) {
            fprintf(out, "typedef struct %s %s;\n", class_c_name(ci), class_c_name(ci));
            ci = ci->next;
        }
        extern InterfaceInfo* interface_list;
        InterfaceInfo* ii = interface_list;
        while (ii) {
            fprintf(out, "typedef struct %s %s;\n", ii->name, ii->name);
            ii = ii->next;
        }
        fprintf(out, "\n");
    }

    codegen_interface_typedefs(&ctx, out);

    AstNode* decl = program->ast_children[0];
    while (decl) {
        if (decl->ast_kind == AST_STRUCT_DECL) {
            codegen_struct_decl(&ctx, decl, out);
        }
        decl = decl->next;
    }

    decl = program->ast_children[0];
    while (decl) {
        if (decl->ast_kind == AST_CLASS_DECL) {
            codegen_class_decl(&ctx, decl, out);
        }
        decl = decl->next;
    }

    /* emit concrete generic class instantiations */
    {
        ClassInfo* ci = class_list;
        while (ci) {
            if (ci->is_instantiation && ci->generic_ast) {
                codegen_class_decl(&ctx, ci->generic_ast, out);
            }
            ci = ci->next;
        }
    }

    decl = program->ast_children[0];
    while (decl) {
        if (decl->ast_kind == AST_FUNC_DECL) {
            codegen_func_decl(&ctx, decl, out);
        }
        decl = decl->next;
    }

    s_last_codegen_error = ctx.codegen_error;

    if (ctx.has_main) {
        fprintf(out, "int main(void) {\n");
        fprintf(out, "#ifdef _MSC_VER\n");
        fprintf(out, "    _set_abort_behavior(0, _WRITE_ABORT_MSG);\n");
        fprintf(out, "    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);\n");
        fprintf(out, "    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);\n");
        fprintf(out, "    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);\n");
        fprintf(out, "#endif\n");
        if (ctx.main_return_type.type_kind == TYPE_VOID) {
            fprintf(out, "    _my_main();\n");
            fprintf(out, "#ifdef _DEBUG\n");
            fprintf(out, "    _CrtDumpMemoryLeaks();\n");
            fprintf(out, "    fflush(stderr);\n");
            fprintf(out, "#endif\n");
            fprintf(out, "    return 0;\n");
        } else {
            fprintf(out, "    %s _ret = _my_main();\n", c_base_name(&ctx.main_return_type));
            fprintf(out, "#ifdef _DEBUG\n");
            fprintf(out, "    _CrtDumpMemoryLeaks();\n");
            fprintf(out, "    fflush(stderr);\n");
            fprintf(out, "#endif\n");
            fprintf(out, "    return (int)_ret;\n");
        }
        fprintf(out, "}\n");
    }
}
