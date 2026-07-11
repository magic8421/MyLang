#include "codegen.h"
#include "symtab.h"
#include "util.h"
#include <string.h>
#include <stdio.h>

static const char* g_source_file = "";
static char        g_source_file_escaped[1024];

static void escape_source_file(const char* src) {
    size_t i, j;
    if (!src) src = "";
    for (i = 0, j = 0; src[i] != '\0' && j < sizeof(g_source_file_escaped) - 2; i++) {
        if (src[i] == '\\' || src[i] == '"') g_source_file_escaped[j++] = '\\';
        g_source_file_escaped[j++] = src[i];
    }
    g_source_file_escaped[j] = '\0';
}

static const char* c_base_name(const Type* t) {
    switch (t->kind) {
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
        case TYPE_CLASS: return t->class_name;
        case TYPE_STRUCT: return t->class_name;
        case TYPE_VOID:  return "void";
        default:         return "int32_t";
    }
}

static void c_type_str(const Type* t, char* buf, int bufsz) {
    int n;
    if (t->array_size > 0) {
        /* fixed-size array declared as T name[N] */
        n = snprintf(buf, bufsz, "%s", c_base_name(t));
    } else if (t->kind == TYPE_CLASS && t->is_array && t->is_pointer) {
        /* dynamic array of class references: Box** */
        n = snprintf(buf, bufsz, "%s**", c_base_name(t));
    } else if (t->is_array && t->array_size == 0) {
        /* dynamic array of primitives: T* */
        n = snprintf(buf, bufsz, "%s*", c_base_name(t));
    } else if (t->kind == TYPE_CLASS || t->is_pointer) {
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

    switch (node->kind) {
        case AST_INT_LIT:
            t.kind = TYPE_I32;
            t.type_id = TYPE_ID_I32;
            break;

        case AST_CHAR_LIT:
            t.kind = TYPE_I8;
            t.type_id = TYPE_ID_I8;
            break;

        case AST_IDENT: {
            SymEntry* e = symtab_lookup(node->tok.text);
            if (e) {
                t = e->type;
                /* A ref parameter is a pointer in C, but its value type
                   for the caller is the referenced object. */
                t.is_ref = 0;
            }
            break;
        }

        case AST_BINARY: {
            TokenKind op = node->tok.kind;
            if (op == TOK_EQ || op == TOK_NE ||
                op == TOK_LT || op == TOK_LE ||
                op == TOK_GT || op == TOK_GE ||
                op == TOK_AND || op == TOK_OR) {
                t.kind = TYPE_I32;
                t.type_id = TYPE_ID_I32;
            } else {
                t = resolve_type(node->children[0]);
            }
            break;
        }

        case AST_UNARY:
            t = resolve_type(node->children[0]);
            break;

        case AST_ARRAY_ACCESS: {
            Type arr = resolve_type(node->children[0]);
            t = arr;
            /* An element is never itself an array. */
            t.is_array = 0;
            t.array_size = 0;
            if (arr.kind == TYPE_CLASS && arr.is_array && arr.is_pointer) {
                /* dynamic class array element is a class pointer */
                t.is_pointer = 1;
            } else {
                t.is_pointer = 0;
            }
            break;
        }

        case AST_MEMBER_ACCESS: {
            Type obj = resolve_type(node->children[0]);
            ClassInfo* ci = symtab_find_class(obj.class_name);
            if (ci) {
                int i;
                for (i = 0; i < ci->field_count; i++) {
                    if (strcmp(ci->field_names[i], node->tok.text) == 0) {
                        t = ci->field_types[i];
                        break;
                    }
                }
            } else {
                StructInfo* si = symtab_find_struct(obj.class_name);
                if (si) {
                    int i;
                    for (i = 0; i < si->field_count; i++) {
                        if (strcmp(si->field_names[i], node->tok.text) == 0) {
                            t = si->field_types[i];
                            break;
                        }
                    }
                }
            }
            break;
        }

        case AST_CALL: {
            if (node->children[0]->kind == AST_MEMBER_ACCESS) {
                AstNode* mem = node->children[0];
                AstNode* obj = mem->children[0];
                resolve_type(obj);
                if (obj->resolved_type.kind == TYPE_CLASS) {
                    MethodInfo* mi = symtab_find_method(obj->resolved_type.class_name, mem->tok.text);
                    if (mi) { t = mi->return_type; }
                }
            } else {
                FuncInfo* fi = symtab_find_func(node->children[0]->tok.text);
                if (fi) { t = fi->return_type; }
            }
            break;
        }

        case AST_NEW: {
            t = node->resolved_type;
            t.is_pointer = 1;
            break;
        }

        case AST_ASSIGN:
            t = resolve_type(node->children[0]);
            break;

        case AST_VAR_DECL:
            t = node->resolved_type;
            break;

        default:
            break;
    }

    node->resolved_type = t;
    return t;
}

static void codegen_expr(AstNode* node, FILE* out);

static void codegen_binary(AstNode* node, FILE* out) {
    fprintf(out, "(");
    codegen_expr(node->children[0], out);
    fprintf(out, " %s ", node->tok.text);
    codegen_expr(node->children[1], out);
    fprintf(out, ")");
}

static void codegen_unary(AstNode* node, FILE* out) {
    fprintf(out, "%s", node->tok.text);
    codegen_expr(node->children[0], out);
}

static int type_is_ref(const Type* t) {
    return t->is_ref;
}

/* Emit a single argument.  If the callee expects a ref parameter, only a
   local variable identifier is allowed; it is passed as &var, or as the raw
   pointer if var itself is already a ref parameter. */
static void codegen_call_arg(AstNode* arg, const Type* param_type, FILE* out) {
    if (type_is_ref(param_type)) {
        if (arg->kind != AST_IDENT) {
            fprintf(stderr, "error at %d:%d: ref argument must be a local variable\n",
                    arg->tok.line, arg->tok.col);
            fprintf(out, "0 /* invalid ref argument */");
            return;
        }
        SymEntry* e = symtab_lookup(arg->tok.text);
        if (!e) {
            fprintf(stderr, "error at %d:%d: ref argument must be a local variable\n",
                    arg->tok.line, arg->tok.col);
            fprintf(out, "0 /* invalid ref argument */");
            return;
        }
        if (type_is_ref(&e->type)) {
            fprintf(out, "%s", arg->tok.text);
        } else {
            fprintf(out, "&%s", arg->tok.text);
        }
    } else {
        codegen_expr(arg, out);
    }
}

static void codegen_call(AstNode* node, FILE* out) {
    /* method call: p.foo(...) ClassName_foo(p, ...) */
    if (node->children[0]->kind == AST_MEMBER_ACCESS) {
        AstNode* mem = node->children[0];
        AstNode* obj = mem->children[0];
        const char* mname = mem->tok.text;
        resolve_type(obj);
        if (obj->resolved_type.kind == TYPE_CLASS) {
            MethodInfo* mi = symtab_find_method(obj->resolved_type.class_name, mname);
            fprintf(out, "%s_%s(", obj->resolved_type.class_name, mname);
            codegen_expr(obj, out);
            AstNode* args = (node->child_count > 1) ? node->children[1] : NULL;
            int idx = 0;
            while (args) {
                fprintf(out, ", ");
                Type expected;
                memset(&expected, 0, sizeof(expected));
                if (mi && idx < mi->param_count) expected = mi->param_types[idx];
                codegen_call_arg(args, &expected, out);
                idx++;
                args = args->next;
            }
            fprintf(out, ")");
            return;
        }
    }
    /* normal function call */
    AstNode* callee = node->children[0];
    FuncInfo* fi = NULL;
    if (callee->kind == AST_IDENT) {
        fi = symtab_find_func(callee->tok.text);
    }
    codegen_expr(callee, out);
    fprintf(out, "(");
    AstNode* args = (node->child_count > 1) ? node->children[1] : NULL;
    int first = 1;
    int idx = 0;
    while (args) {
        if (!first) fprintf(out, ", ");
        Type expected;
        memset(&expected, 0, sizeof(expected));
        if (fi && idx < fi->param_count) expected = fi->param_types[idx];
        codegen_call_arg(args, &expected, out);
        first = 0;
        idx++;
        args = args->next;
    }
    fprintf(out, ")");
}

static void codegen_array_access(AstNode* node, FILE* out) {
    AstNode* arr = node->children[0];
    AstNode* idx = node->children[1];
    resolve_type(arr);
    Type at = arr->resolved_type;
    if (at.is_array && at.array_size == 0) {
        /* dynamic array: use bounds-checked getter */
        Type et = resolve_type(node);
        if (et.kind == TYPE_CLASS) {
            fprintf(out, "array_get_class(");
            codegen_expr(arr, out);
            fprintf(out, ", ");
            codegen_expr(idx, out);
            fprintf(out, ", \"%s\", %d)", g_source_file_escaped, node->tok.line);
        } else if (et.kind == TYPE_STRUCT) {
            fprintf(out, "(*(%s*)array_get_struct_ptr(", c_base_name(&et));
            codegen_expr(arr, out);
            fprintf(out, ", ");
            codegen_expr(idx, out);
            fprintf(out, ", sizeof(%s), \"%s\", %d))", c_base_name(&et), g_source_file_escaped, node->tok.line);
        } else {
            fprintf(out, "array_get_%s(", c_base_name(&et));
            codegen_expr(arr, out);
            fprintf(out, ", ");
            codegen_expr(idx, out);
            fprintf(out, ", \"%s\", %d)", g_source_file_escaped, node->tok.line);
        }
    } else {
        codegen_expr(arr, out);
        fprintf(out, "[");
        codegen_expr(idx, out);
        fprintf(out, "]");
    }
}

static void codegen_member_access(AstNode* node, FILE* out) {
    AstNode* obj = node->children[0];
    resolve_type(obj);
    if (obj->resolved_type.kind == TYPE_CLASS) {
        /* Class references may come from void* getters (e.g. dynamic arrays),
           so cast to the concrete struct pointer before using ->. */
        fprintf(out, "((%s*)", obj->resolved_type.class_name);
        codegen_expr(obj, out);
        fprintf(out, ")->%s", node->tok.text);
    } else if (obj->resolved_type.is_pointer) {
        codegen_expr(obj, out);
        fprintf(out, "->%s", node->tok.text);
    } else {
        codegen_expr(obj, out);
        fprintf(out, ".%s", node->tok.text);
    }
}

static void codegen_new(AstNode* node, FILE* out) {
    Type base = node->resolved_type;
    if (node->child_count > 0) {
        int is_class = (base.kind == TYPE_CLASS) ? 1 : 0;
        fprintf(out, "mylang_new_array(");
        codegen_expr(node->children[0], out);
        fprintf(out, ", ");
        if (is_class) {
            fprintf(out, "sizeof(void*)");
        } else {
            fprintf(out, "sizeof(%s)", c_base_name(&base));
        }
        fprintf(out, ", 0x%08XU)", (unsigned)(base.type_id | TYPE_IS_ARRAY));
        base.is_pointer = 1;
    } else {
        if (base.kind == TYPE_CLASS) {
            fprintf(out, "mylang_new_object(sizeof(%s), %u)", c_base_name(&base), (unsigned)base.type_id);
        } else {
            fprintf(out, "calloc(1, sizeof(%s))", c_base_name(&base));
        }
    }
    node->resolved_type = base;
}

static void codegen_char_lit(AstNode* node, FILE* out) {
    char c = node->tok.char_val;
    switch (c) {
        case '\n': fprintf(out, "'\\n'"); break;
        case '\t': fprintf(out, "'\\t'"); break;
        case '\r': fprintf(out, "'\\r'"); break;
        case '\\': fprintf(out, "'\\\\'"); break;
        case '\'': fprintf(out, "'\\''"); break;
        default:   fprintf(out, "'%c'", c); break;
    }
}

static void codegen_expr(AstNode* node, FILE* out) {
    if (!node) return;
    resolve_type(node);

    if (node->temp_name[0] != '\0') {
        fprintf(out, "%s", node->temp_name);
        return;
    }

    switch (node->kind) {
        case AST_INT_LIT:
            fprintf(out, "%d", node->tok.int_val);
            break;
        case AST_CHAR_LIT:
            codegen_char_lit(node, out);
            break;
        case AST_IDENT: {
            if (strcmp(node->tok.text, "this") == 0) {
                fprintf(out, "thiz");
            } else {
                SymEntry* e = symtab_lookup(node->tok.text);
                if (e && type_is_ref(&e->type)) {
                    fprintf(out, "(*%s)", node->tok.text);
                } else {
                    fprintf(out, "%s", node->tok.text);
                }
            }
            break;
        }
        case AST_BINARY:
            codegen_binary(node, out);
            break;
        case AST_UNARY:
            codegen_unary(node, out);
            break;
        case AST_ASSIGN: {
            AstNode* lhs = node->children[0];
            AstNode* rhs = node->children[1];
            Type lt = lhs->resolved_type;

            if (lhs->kind == AST_ARRAY_ACCESS) {
                Type at = lhs->children[0]->resolved_type;
                if (at.is_array && at.array_size == 0) {
                    /* dynamic array element assignment */
                    AstNode* arr = lhs->children[0];
                    AstNode* idx = lhs->children[1];
                    int is_class_elem = (lt.kind == TYPE_CLASS && lt.is_pointer);
                    int is_struct_elem = (lt.kind == TYPE_STRUCT);
                    int rhs_owned = (rhs->kind == AST_CALL || rhs->kind == AST_NEW);

                    if (is_struct_elem) {
                        fprintf(out, "(*(%s*)array_get_struct_ptr(", c_base_name(&lt));
                        codegen_expr(arr, out);
                        fprintf(out, ", ");
                        codegen_expr(idx, out);
                        fprintf(out, ", sizeof(%s), \"%s\", %d)) = ", c_base_name(&lt), g_source_file_escaped, node->tok.line);
                        codegen_expr(rhs, out);
                    } else if (is_class_elem) {
                        fprintf(out, "array_replace_class(");
                        codegen_expr(arr, out);
                        fprintf(out, ", ");
                        codegen_expr(idx, out);
                        fprintf(out, ", ");
                        if (!rhs_owned) {
                            fprintf(out, "mylang_retain(");
                            codegen_expr(rhs, out);
                            fprintf(out, ")");
                        } else {
                            codegen_expr(rhs, out);
                        }
                        fprintf(out, ", \"%s\", %d)", g_source_file_escaped, node->tok.line);
                    } else {
                        fprintf(out, "array_set_%s(", c_base_name(&lt));
                        codegen_expr(arr, out);
                        fprintf(out, ", ");
                        codegen_expr(idx, out);
                        fprintf(out, ", ");
                        codegen_expr(rhs, out);
                        fprintf(out, ", \"%s\", %d)", g_source_file_escaped, node->tok.line);
                    }
                    break;
                }
            }

            if (lt.kind == TYPE_CLASS) {
                int rhs_owned = (rhs->kind == AST_CALL || rhs->kind == AST_NEW);
                int rhs_local = (rhs->kind == AST_IDENT && symtab_lookup(rhs->tok.text) != NULL);

                fprintf(out, "((");
                if (!rhs_owned && !rhs_local) {
                    fprintf(out, "(void)mylang_retain(");
                    codegen_expr(rhs, out);
                    fprintf(out, "), ");
                }
                fprintf(out, "(void)mylang_release(");
                codegen_expr(lhs, out);
                fprintf(out, "), (");
                codegen_expr(lhs, out);
                fprintf(out, " = ");
                codegen_expr(rhs, out);
                fprintf(out, ")))");
            } else {
                codegen_expr(lhs, out);
                fprintf(out, " = ");
                codegen_expr(rhs, out);
            }
            break;
        }
        case AST_CALL:
            codegen_call(node, out);
            break;
        case AST_ARRAY_ACCESS:
            codegen_array_access(node, out);
            break;
        case AST_MEMBER_ACCESS:
            codegen_member_access(node, out);
            break;
        case AST_NEW:
            codegen_new(node, out);
            break;
        default:
            fprintf(out, "/* ??? */");
            break;
    }
}

/* -- bounds checking ------------------------------------------------- */

static void indent_line(FILE* out, int indent);

static void emit_bounds_checks(AstNode* expr, FILE* out, int indent) {
    if (!expr) return;

    if (expr->kind == AST_ARRAY_ACCESS) {
        AstNode* arr = expr->children[0];
        AstNode* idx = expr->children[1];

        emit_bounds_checks(arr, out, indent);
        emit_bounds_checks(idx, out, indent);

        resolve_type(arr);
        Type at = arr->resolved_type;

        /* Dynamic arrays use runtime bounds inside getter/setter helpers. */
        if (at.array_size > 0) {
            indent_line(out, indent);
            fprintf(out, "MY_LOC(\"%s\", %d); MY_CHECK((size_t)(", g_source_file_escaped, expr->tok.line);
            codegen_expr(idx, out);
            fprintf(out, ") < %d, \"index out of bounds\");\n", at.array_size);
        }
    } else {
        int i;
        for (i = 0; i < expr->child_count; i++) {
            emit_bounds_checks(expr->children[i], out, indent);
        }
    }
    emit_bounds_checks(expr->next, out, indent);
}

/* -- caller-side arg guard helpers ----------------------------------- */

static int guard_tmp_id = 0;

static int call_needs_guard(AstNode* arg) {
    resolve_type(arg);
    if (arg->resolved_type.kind != TYPE_CLASS) return 0;
    if (arg->kind == AST_ASSIGN) return 0;
    if (arg->kind == AST_IDENT && symtab_lookup(arg->tok.text)) return 0;
    return 1;
}

static int guard_needs_retain(AstNode* node) {
    /* Calls/new already return an owned (+1) reference. */
    return node->kind != AST_CALL && node->kind != AST_NEW;
}

/* Evaluate each guarded class subexpression into a temporary once, so we do
   not re-evaluate side-effecting expressions (e.g. method calls) when the
   caller-side retain/release guards are emitted. */
static void emit_guarded_temp_decls(AstNode* expr, FILE* out, int indent) {
    if (!expr) return;

    /* Do not extract the LHS of an assignment into a temporary; it must remain
       an lvalue so the assignment writes to the real location. */
    if (expr->kind == AST_ASSIGN) {
        emit_guarded_temp_decls(expr->children[1], out, indent);
        emit_guarded_temp_decls(expr->next, out, indent);
        return;
    }

    int i;
    for (i = 0; i < expr->child_count; i++) {
        emit_guarded_temp_decls(expr->children[i], out, indent);
    }
    emit_guarded_temp_decls(expr->next, out, indent);

    if (call_needs_guard(expr) && expr->temp_name[0] == '\0') {
        int id = guard_tmp_id++;
        int n = snprintf(expr->temp_name, sizeof(expr->temp_name), "_g%d", id);
        CHECK_SNPRINTF(n, sizeof(expr->temp_name), "guard temporary name too long");

        char tbuf[128];
        c_type_str(&expr->resolved_type, tbuf, sizeof(tbuf));
        indent_line(out, indent);
        fprintf(out, "%s %s = ", tbuf, expr->temp_name);

        /* Evaluate the original expression without using its own temp name. */
        char saved[64];
        CHECK_STRSCPY(strscpy(saved, expr->temp_name, sizeof(saved)),
                      "guard temporary name too long");
        expr->temp_name[0] = '\0';
        codegen_expr(expr, out);
        CHECK_STRSCPY(strscpy(expr->temp_name, saved, sizeof(expr->temp_name)),
                      "guard temporary name too long");

        fprintf(out, ";\n");
    }
}

static void emit_call_guards(AstNode* expr, FILE* out, int is_retain) {
    if (!expr) return;
    if (expr->kind == AST_CALL) {
        AstNode* callee = expr->children[0];
        AstNode* args  = (expr->child_count > 1) ? expr->children[1] : NULL;

        if (callee->kind == AST_MEMBER_ACCESS) {
            AstNode* obj = callee->children[0];
            if (call_needs_guard(obj)) {
                if (is_retain) {
                    if (guard_needs_retain(obj)) fprintf(out, "mylang_retain(%s); ", obj->temp_name);
                } else {
                    fprintf(out, "mylang_release(%s); ", obj->temp_name);
                }
            }
        }
        AstNode* a = args;
        while (a) {
            if (call_needs_guard(a)) {
                if (is_retain) {
                    if (guard_needs_retain(a)) fprintf(out, "mylang_retain(%s); ", a->temp_name);
                } else {
                    fprintf(out, "mylang_release(%s); ", a->temp_name);
                }
            }
            a = a->next;
        }
    }
    int i;
    for (i = 0; i < expr->child_count; i++) emit_call_guards(expr->children[i], out, is_retain);
}

static void emit_stmt_call_retains(AstNode* expr, FILE* out, int indent) {
    indent_line(out, indent);
    emit_call_guards(expr, out, 1);
    fprintf(out, "\n");
}
static void emit_stmt_call_releases(AstNode* expr, FILE* out, int indent) {
    indent_line(out, indent);
    emit_call_guards(expr, out, 0);
    fprintf(out, "\n");
}

/* --- CODE GENERATION -- statements --- */

/* -- refcount cleanup list ------------------------------------------- */

#define MAX_CLEANUP 128
typedef struct {
    const char* name;
} CleanupEntry;
static CleanupEntry cleanup_entries[MAX_CLEANUP];
static int          cleanup_count = 0;
static int          assign_tmp_id = 0;

#define MAX_SCOPE 64
static int cleanup_scope_stack[MAX_SCOPE];
static int cleanup_scope_depth = 0;

static void cleanup_add(const char* name) {
    if (cleanup_count < MAX_CLEANUP) {
        cleanup_entries[cleanup_count].name = name;
        cleanup_count++;
    }
}

static void cleanup_emit(FILE* out, int indent) {
    int i;
    for (i = cleanup_count - 1; i >= 0; i--) {
        const char* name = cleanup_entries[i].name;
        indent_line(out, indent);
        fprintf(out, "mylang_release(%s);\n",
                strcmp(name, "this") == 0 ? "thiz" : name);
    }
}

static void cleanup_push_scope(void) {
    if (cleanup_scope_depth < MAX_SCOPE) {
        cleanup_scope_stack[cleanup_scope_depth++] = cleanup_count;
    }
}

static void cleanup_pop_scope(FILE* out, int indent) {
    if (cleanup_scope_depth == 0) return;
    int saved = cleanup_scope_stack[--cleanup_scope_depth];
    int i;
    for (i = cleanup_count - 1; i >= saved; i--) {
        const char* name = cleanup_entries[i].name;
        indent_line(out, indent);
        fprintf(out, "mylang_release(%s);\n",
                strcmp(name, "this") == 0 ? "thiz" : name);
    }
    cleanup_count = saved;
}

static void codegen_stmt(AstNode* node, FILE* out, int indent);

static void indent_line(FILE* out, int indent) {
    int i;
    for (i = 0; i < indent; i++) fprintf(out, "    ");
}

static void codegen_body(AstNode* body, FILE* out, int indent) {
    indent_line(out, indent);
    fprintf(out, "{\n");
    cleanup_push_scope();
    if (body->kind == AST_BLOCK) {
        AstNode* s = body->children[0];
        while (s) {
            codegen_stmt(s, out, indent + 1);
            s = s->next;
        }
    } else {
        codegen_stmt(body, out, indent + 1);
    }
    cleanup_pop_scope(out, indent + 1);
    indent_line(out, indent);
    fprintf(out, "}\n");
}

static void codegen_var_decl(AstNode* node, FILE* out, int indent) {
    Type type = node->resolved_type;

    symtab_insert(node->tok.text, type);

    indent_line(out, indent);

    if (type.array_size > 0) {
        fprintf(out, "%s %s[%d]", c_base_name(&type), node->tok.text, type.array_size);
    } else {
        char typename_buf[128];
        c_type_str(&type, typename_buf, sizeof(typename_buf));
        fprintf(out, "%s %s", typename_buf, node->tok.text);
    }

    if (node->child_count > 0) {
        if (type.kind == TYPE_CLASS && node->children[0]->kind != AST_NEW
            && node->children[0]->kind != AST_CALL) {
            fprintf(out, " = mylang_retain(");
            codegen_expr(node->children[0], out);
            fprintf(out, ")");
        } else {
            fprintf(out, " = ");
            codegen_expr(node->children[0], out);
        }
    } else if (type.is_pointer) {
        fprintf(out, " = NULL");
    }
    fprintf(out, ";\n");

    if ((type.kind == TYPE_CLASS && type.is_pointer && !type.is_array) ||
        (type.is_array && type.array_size == 0)) {
        cleanup_add(node->tok.text);
    }
}

static void codegen_if_stmt(AstNode* node, FILE* out, int indent) {
    emit_bounds_checks(node->children[0], out, indent);
    indent_line(out, indent);
    fprintf(out, "if (");
    codegen_expr(node->children[0], out);
    fprintf(out, ")\n");
    codegen_body(node->children[1], out, indent);

    if (node->child_count > 2) {
        indent_line(out, indent);
        fprintf(out, "else\n");
        codegen_body(node->children[2], out, indent);
    }
}

static void codegen_while_stmt(AstNode* node, FILE* out, int indent) {
    emit_bounds_checks(node->children[0], out, indent);
    indent_line(out, indent);
    fprintf(out, "while (");
    codegen_expr(node->children[0], out);
    fprintf(out, ")\n");
    codegen_body(node->children[1], out, indent);
}

static void codegen_return_stmt(AstNode* node, FILE* out, int indent) {
    if (node->child_count > 0) {
        AstNode* ret = node->children[0];
        emit_bounds_checks(ret, out, indent);
        resolve_type(ret);

        if (ret->resolved_type.kind == TYPE_CLASS) {
            indent_line(out, indent);
            fprintf(out, "void* _r = mylang_retain(");
            codegen_expr(ret, out);
            fprintf(out, ");\n");
            cleanup_emit(out, indent);
            indent_line(out, indent);
            fprintf(out, "MY_POP();\n");
            indent_line(out, indent);
            fprintf(out, "return _r;\n");
        } else {
            char tbuf[128];
            c_type_str(&ret->resolved_type, tbuf, sizeof(tbuf));
            indent_line(out, indent);
            fprintf(out, "%s _mylang_ret = ", tbuf);
            codegen_expr(ret, out);
            fprintf(out, ";\n");
            cleanup_emit(out, indent);
            indent_line(out, indent);
            fprintf(out, "MY_POP();\n");
            indent_line(out, indent);
            fprintf(out, "return _mylang_ret;\n");
        }
    } else {
        cleanup_emit(out, indent);
        indent_line(out, indent);
        fprintf(out, "MY_POP();\n");
        indent_line(out, indent);
        fprintf(out, "return;\n");
    }
}

static void codegen_expr_stmt(AstNode* node, FILE* out, int indent) {
    emit_bounds_checks(node->children[0], out, indent);
    AstNode* expr = node->children[0];
    resolve_type(expr);

    /* Extract guarded class subexpressions into temporaries so side-effecting
       arguments (e.g. method calls) are evaluated exactly once. */
    emit_guarded_temp_decls(expr, out, indent);

    /* class/array assignment: evaluate RHS first, release old LHS, then assign.
       This avoids use-after-free when RHS aliases LHS (e.g. b = b.set(5)). */
    if (expr->kind == AST_ASSIGN) {
        AstNode* lhs = expr->children[0];
        AstNode* rhs = expr->children[1];
        resolve_type(lhs);
        resolve_type(rhs);
        Type lt = lhs->resolved_type;

        if (lhs->kind == AST_ARRAY_ACCESS) {
            Type at = lhs->children[0]->resolved_type;
            if (at.is_array && at.array_size == 0) {
                emit_stmt_call_retains(expr, out, indent);

                int is_class_elem = (lt.kind == TYPE_CLASS && lt.is_pointer);
                AstNode* arr = lhs->children[0];
                AstNode* idx = lhs->children[1];
                int rhs_owned = (rhs->kind == AST_CALL || rhs->kind == AST_NEW);

                indent_line(out, indent);
                if (is_class_elem) {
                    fprintf(out, "array_replace_class(");
                } else {
                    fprintf(out, "array_set_%s(", c_base_name(&lt));
                }
                codegen_expr(arr, out);
                fprintf(out, ", ");
                codegen_expr(idx, out);
                fprintf(out, ", ");
                if (is_class_elem && !rhs_owned) {
                    fprintf(out, "mylang_retain(");
                    codegen_expr(rhs, out);
                    fprintf(out, ")");
                } else {
                    codegen_expr(rhs, out);
                }
                fprintf(out, ", \"%s\", %d);\n", g_source_file_escaped, expr->tok.line);

                emit_stmt_call_releases(expr, out, indent);
                return;
            }
        }

        if (lhs->kind == AST_IDENT && (lt.kind == TYPE_CLASS || lt.is_array)) {
            emit_stmt_call_retains(expr, out, indent);

            int id = assign_tmp_id++;
            int rhs_owned = (rhs->kind == AST_CALL || rhs->kind == AST_NEW);

            indent_line(out, indent);
            fprintf(out, "void* _my_assign_%d = ", id);
            if (!rhs_owned) {
                fprintf(out, "mylang_retain(");
                codegen_expr(rhs, out);
                fprintf(out, ")");
            } else {
                codegen_expr(rhs, out);
            }
            fprintf(out, ";\n");

            indent_line(out, indent);
            fprintf(out, "mylang_release(");
            codegen_expr(lhs, out);
            fprintf(out, ");\n");

            indent_line(out, indent);
            codegen_expr(lhs, out);
            fprintf(out, " = _my_assign_%d;\n", id);

            emit_stmt_call_releases(expr, out, indent);
            return;
        }
    }

    emit_stmt_call_retains(expr, out, indent);

    indent_line(out, indent);
    if (expr->kind == AST_CALL && expr->resolved_type.kind == TYPE_CLASS) {
        /* discarded class return: release the +1 from callee */
        fprintf(out, "(void)mylang_release(");
        codegen_expr(expr, out);
        fprintf(out, ")");
    } else {
        codegen_expr(expr, out);
    }
    fprintf(out, ";\n");

    emit_stmt_call_releases(expr, out, indent);
}

static void codegen_stmt(AstNode* node, FILE* out, int indent) {
    if (!node) return;

    switch (node->kind) {
        case AST_BLOCK:
            codegen_body(node, out, indent);
            break;
        case AST_VAR_DECL:
            codegen_var_decl(node, out, indent);
            break;
        case AST_IF_STMT:
            codegen_if_stmt(node, out, indent);
            break;
        case AST_WHILE_STMT:
            codegen_while_stmt(node, out, indent);
            break;
        case AST_RETURN_STMT:
            codegen_return_stmt(node, out, indent);
            break;
        case AST_EXPR_STMT:
            codegen_expr_stmt(node, out, indent);
            break;
        default:
            indent_line(out, indent);
            fprintf(out, "/* unknown stmt kind=%d */\n", node->kind);
            break;
    }
}


static void codegen_method_decl(AstNode* node, FILE* out, const char* class_name);

static void codegen_struct_decl(AstNode* node, FILE* out) {
    StructInfo* si = symtab_find_struct(node->tok.text);
    if (!si) return;

    fprintf(out, "typedef struct {\n");
    int i;
    for (i = 0; i < si->field_count; i++) {
        char ftype_buf[128];
        c_type_str(&si->field_types[i], ftype_buf, sizeof(ftype_buf));
        fprintf(out, "    %s %s;\n", ftype_buf, si->field_names[i]);
    }
    fprintf(out, "} %s;\n\n", node->tok.text);
}

static void codegen_class_decl(AstNode* node, FILE* out) {
    ClassInfo* ci = symtab_find_class(node->tok.text);
    if (!ci) return;

    fprintf(out, "typedef struct {\n");
    int i;
    for (i = 0; i < ci->field_count; i++) {
        char ftype_buf[128];
        c_type_str(&ci->field_types[i], ftype_buf, sizeof(ftype_buf));
        fprintf(out, "    %s %s;\n", ftype_buf, ci->field_names[i]);
    }
    fprintf(out, "} %s;\n\n", node->tok.text);

    /* emit method declarations */
    AstNode* m = node->children[0];
    while (m) {
        codegen_method_decl(m, out, node->tok.text);
        m = m->next;
    }
}


static void codegen_method_decl(AstNode* node, FILE* out, const char* class_name) {
    char ret_buf[128];
    c_type_str(&node->resolved_type, ret_buf, sizeof(ret_buf));
    fprintf(out, "%s %s_%s(%s* thiz", ret_buf, class_name, node->tok.text, class_name);
    AstNode* params = NULL; AstNode* body = NULL;
    if (node->child_count == 2) { params = node->children[0]; body = node->children[1]; }
    else { body = node->children[0]; }
    { AstNode* p = params; while (p) { fprintf(out, ", ");
        char pt[128]; c_type_str(&p->resolved_type, pt, sizeof(pt));
        if (type_is_ref(&p->resolved_type)) {
            fprintf(out, "%s* %s", pt, p->tok.text);
        } else {
            fprintf(out, "%s %s", pt, p->tok.text);
        }
        p = p->next; } }
    fprintf(out, ")\n{\n");
    cleanup_push_scope(); symtab_enter_scope();
    Type thiz_type; memset(&thiz_type, 0, sizeof(thiz_type));
    thiz_type.kind = TYPE_CLASS;
    CHECK_STRSCPY(strscpy(thiz_type.class_name, class_name, sizeof(thiz_type.class_name)), "class name too long");
    thiz_type.is_pointer = 1; symtab_insert("this", thiz_type);
    { AstNode* p = params; while (p) { symtab_insert(p->tok.text, p->resolved_type);
        p = p->next; } }

    indent_line(out, 1);
    fprintf(out, "MY_PUSH(\"%s.%s\", \"%s\", %d);\n", class_name, node->tok.text, g_source_file_escaped, node->tok.line);

    fprintf(out, "{\n");
    if (body && body->kind == AST_BLOCK) { AstNode* s = body->children[0];
        while (s) { codegen_stmt(s, out, 2); s = s->next; } }
    cleanup_pop_scope(out, 2);
    fprintf(out, "}\n");

    cleanup_pop_scope(out, 1);
    symtab_exit_scope();
    indent_line(out, 1);
    fprintf(out, "MY_POP();\n");
    fprintf(out, "}\n\n");
}
static void codegen_func_decl(AstNode* node, FILE* out) {
    /* return type */
    {
        char ret_buf[128];
        c_type_str(&node->resolved_type, ret_buf, sizeof(ret_buf));
        fprintf(out, "%s %s(", ret_buf, node->tok.text);
    }

    /* parameters */
    AstNode* params = NULL;
    AstNode* body   = NULL;

    if (node->child_count == 2) {
        params = node->children[0];
        body   = node->children[1];
    } else {
        body = node->children[0];
    }

    {
        AstNode* p = params;
        int first = 1;
        while (p) {
            if (!first) fprintf(out, ", ");
            char ptype_buf[128];
            c_type_str(&p->resolved_type, ptype_buf, sizeof(ptype_buf));
            if (type_is_ref(&p->resolved_type)) {
                fprintf(out, "%s* %s", ptype_buf, p->tok.text);
            } else {
                fprintf(out, "%s %s", ptype_buf, p->tok.text);
            }
            first = 0;
            p = p->next;
        }
    }
    fprintf(out, ")\n");

    /* body */
    cleanup_push_scope();
    fprintf(out, "{\n");

    symtab_enter_scope();

    /* register parameters in scope */
    {
        AstNode* p = params;
        while (p) {
            symtab_insert(p->tok.text, p->resolved_type);
            p = p->next;
        }
    }

    indent_line(out, 1);
    fprintf(out, "MY_PUSH(\"%s\", \"%s\", %d);\n", node->tok.text, g_source_file_escaped, node->tok.line);

    fprintf(out, "{\n");

    /* walk body statements */
    if (body && body->kind == AST_BLOCK) {
        AstNode* s = body->children[0];
        while (s) {
            codegen_stmt(s, out, 2);
            s = s->next;
        }
    }

    cleanup_pop_scope(out, 2);
    fprintf(out, "}\n");

    cleanup_pop_scope(out, 1);
    symtab_exit_scope();
    indent_line(out, 1);
    fprintf(out, "MY_POP();\n");
    fprintf(out, "}\n\n");
}

void codegen_program(AstNode* program, FILE* out, const char* source_file) {
    g_source_file = source_file ? source_file : "";
    escape_source_file(g_source_file);
    fprintf(out, "/* Generated by MyLang compiler */\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stddef.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#ifdef _MSC_VER\n");
    fprintf(out, "#include <intrin.h>\n");
    fprintf(out, "#else\n");
    fprintf(out, "#define __debugbreak() __builtin_trap()\n");
    fprintf(out, "#endif\n\n");

    fprintf(out, "#define MYLANG_TYPE_IS_ARRAY   0x80000000U\n");
    fprintf(out, "#define MYLANG_TYPE_IS_STRUCT  0x40000000U\n");
    fprintf(out, "#define MYLANG_TYPE_ID_CLASS_BASE 16\n\n");

    fprintf(out, "#ifdef _MSC_VER\n");
    fprintf(out, "#include <windows.h>\n");
    fprintf(out, "#define mylang_atomic_inc(p) InterlockedIncrement(p)\n");
    fprintf(out, "#define mylang_atomic_dec(p) InterlockedDecrement(p)\n");
    fprintf(out, "#else\n");
    fprintf(out, "#include <stdatomic.h>\n");
    fprintf(out, "#define mylang_atomic_inc(p) (atomic_fetch_add(p, 1) + 1)\n");
    fprintf(out, "#define mylang_atomic_dec(p) (atomic_fetch_sub(p, 1) - 1)\n");
    fprintf(out, "#endif\n\n");

    fprintf(out, "#ifdef _MSC_VER\n");
    fprintf(out, "#define MY_TL __declspec(thread)\n");
    fprintf(out, "#else\n");
    fprintf(out, "#define MY_TL _Thread_local\n");
    fprintf(out, "#endif\n\n");

    fprintf(out, "typedef struct {\n");
    fprintf(out, "    const char* func;\n");
    fprintf(out, "    const char* file;\n");
    fprintf(out, "    int         line;\n");
    fprintf(out, "} MyFrame;\n\n");
    fprintf(out, "#define MY_STACK_MAX 128\n\n");
    fprintf(out, "MY_TL const char* __my_file = NULL;\n");
    fprintf(out, "MY_TL int         __my_line = 0;\n");
    fprintf(out, "MY_TL MyFrame     __my_stack[MY_STACK_MAX];\n");
    fprintf(out, "MY_TL int         __my_depth = 0;\n\n");
    fprintf(out, "#define MY_LOC(f, l) do { __my_file = (f); __my_line = (l); } while(0)\n");
    fprintf(out, "#define MY_PUSH(fn, f, l) do { \\\n");
    fprintf(out, "    if (__my_depth < MY_STACK_MAX) { \\\n");
    fprintf(out, "        MyFrame* _fr = &__my_stack[__my_depth++]; \\\n");
    fprintf(out, "        _fr->func = (fn); _fr->file = (f); _fr->line = (l); \\\n");
    fprintf(out, "    } \\\n");
    fprintf(out, "} while(0)\n");
    fprintf(out, "#define MY_POP() do { if (__my_depth > 0) __my_depth--; } while(0)\n\n");
    fprintf(out, "static inline void my_backtrace(void) {\n");
    fprintf(out, "    if (__my_depth == 0) {\n");
    fprintf(out, "        fprintf(stderr, \"  (stack empty)\\n\");\n");
    fprintf(out, "        return;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    fprintf(stderr, \"Backtrace (most recent call first):\\n\");\n");
    fprintf(out, "    { int _i; for (_i = __my_depth - 1; _i >= 0; _i--) {\n");
    fprintf(out, "        MyFrame* _fr = &__my_stack[_i];\n");
    fprintf(out, "        fprintf(stderr, \"  #%%d %%s at %%s:%%d\\n\", __my_depth - 1 - _i,\n");
    fprintf(out, "                _fr->func ? _fr->func : \"???\",\n");
    fprintf(out, "                _fr->file ? _fr->file : \"???\",\n");
    fprintf(out, "                _fr->line);\n");
    fprintf(out, "    } }\n");
    fprintf(out, "}\n\n");
    fprintf(out, "static inline void my_panic(const char* msg) {\n");
    fprintf(out, "    fprintf(stderr, \"Panic: %%s\\n\", msg);\n");
    fprintf(out, "    fprintf(stderr, \"  -> triggered at %%s:%%d\\n\",\n");
    fprintf(out, "            __my_file ? __my_file : \"?\", __my_line);\n");
    fprintf(out, "    my_backtrace();\n");
    fprintf(out, "    abort();\n");
    fprintf(out, "}\n\n");
    fprintf(out, "#define MY_CHECK(c, m) do { if (!(c)) my_panic(m); } while(0)\n\n");

    fprintf(out, "typedef struct { volatile long refcount; uint32_t type_id; size_t length; } ObjHeader;\n");
    fprintf(out, "#define mylang_obj_hdr(ptr) ((ObjHeader*)((char*)(ptr) - sizeof(ObjHeader)))\n\n");

    fprintf(out, "static void* mylang_new_object(size_t sz, uint32_t type_id) {\n");
    fprintf(out, "    ObjHeader* h = calloc(1, sizeof(ObjHeader) + sz);\n");
    fprintf(out, "    h->refcount = 1;\n");
    fprintf(out, "    h->type_id = type_id;\n");
    fprintf(out, "    return h + 1;\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static void* mylang_new_array(size_t count, size_t elem_size, uint32_t type_id) {\n");
    fprintf(out, "    ObjHeader* h = calloc(1, sizeof(ObjHeader) + count * elem_size);\n");
    fprintf(out, "    h->refcount = 1;\n");
    fprintf(out, "    h->type_id = type_id;\n");
    fprintf(out, "    h->length = count;\n");
    fprintf(out, "    return h + 1;\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static inline void mylang_check_bounds(void* arr, size_t idx, const char* file, int line) {\n");
    fprintf(out, "    MY_LOC(file, line);\n");
    fprintf(out, "    MY_CHECK((size_t)(idx) < mylang_obj_hdr(arr)->length, \"index out of bounds\");\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static void* mylang_retain(void* ptr) {\n");
    fprintf(out, "    if (ptr) mylang_atomic_inc(&mylang_obj_hdr(ptr)->refcount);\n");
    fprintf(out, "    return ptr;\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static int mylang_release(void* ptr) {\n");
    fprintf(out, "    if (ptr && mylang_atomic_dec(&mylang_obj_hdr(ptr)->refcount) == 0) {\n");
    fprintf(out, "        ObjHeader* h = mylang_obj_hdr(ptr);\n");
    fprintf(out, "        if (h->type_id & MYLANG_TYPE_IS_ARRAY) {\n");
    fprintf(out, "            if (!(h->type_id & MYLANG_TYPE_IS_STRUCT)) {\n");
    fprintf(out, "                uint32_t et = h->type_id & ~MYLANG_TYPE_IS_ARRAY;\n");
    fprintf(out, "                if (et >= MYLANG_TYPE_ID_CLASS_BASE) {\n");
    fprintf(out, "                    void** data = (void**)ptr;\n");
    fprintf(out, "                    size_t i;\n");
    fprintf(out, "                    for (i = 0; i < h->length; i++) mylang_release(data[i]);\n");
    fprintf(out, "                }\n");
    fprintf(out, "            }\n");
    fprintf(out, "        }\n");
    fprintf(out, "        free(h);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static inline int8_t   array_get_int8_t  (void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((int8_t*)arr)[idx];   }\n");
    fprintf(out, "static inline void     array_set_int8_t  (void* arr, size_t idx, int8_t   val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((int8_t*)arr)[idx]   = val; }\n");
    fprintf(out, "static inline int16_t  array_get_int16_t (void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((int16_t*)arr)[idx];  }\n");
    fprintf(out, "static inline void     array_set_int16_t (void* arr, size_t idx, int16_t  val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((int16_t*)arr)[idx]  = val; }\n");
    fprintf(out, "static inline int32_t  array_get_int32_t (void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((int32_t*)arr)[idx];  }\n");
    fprintf(out, "static inline void     array_set_int32_t (void* arr, size_t idx, int32_t  val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((int32_t*)arr)[idx]  = val; }\n");
    fprintf(out, "static inline int64_t  array_get_int64_t (void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((int64_t*)arr)[idx];  }\n");
    fprintf(out, "static inline void     array_set_int64_t (void* arr, size_t idx, int64_t  val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((int64_t*)arr)[idx]  = val; }\n");
    fprintf(out, "static inline uint8_t  array_get_uint8_t (void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((uint8_t*)arr)[idx];  }\n");
    fprintf(out, "static inline void     array_set_uint8_t (void* arr, size_t idx, uint8_t  val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((uint8_t*)arr)[idx]  = val; }\n");
    fprintf(out, "static inline uint16_t array_get_uint16_t(void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((uint16_t*)arr)[idx]; }\n");
    fprintf(out, "static inline void     array_set_uint16_t(void* arr, size_t idx, uint16_t val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((uint16_t*)arr)[idx] = val; }\n");
    fprintf(out, "static inline uint32_t array_get_uint32_t(void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((uint32_t*)arr)[idx]; }\n");
    fprintf(out, "static inline void     array_set_uint32_t(void* arr, size_t idx, uint32_t val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((uint32_t*)arr)[idx] = val; }\n");
    fprintf(out, "static inline uint64_t array_get_uint64_t(void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((uint64_t*)arr)[idx]; }\n");
    fprintf(out, "static inline void     array_set_uint64_t(void* arr, size_t idx, uint64_t val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((uint64_t*)arr)[idx] = val; }\n");
    fprintf(out, "static inline float    array_get_float   (void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((float*)arr)[idx];    }\n");
    fprintf(out, "static inline void     array_set_float   (void* arr, size_t idx, float    val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((float*)arr)[idx]    = val; }\n");
    fprintf(out, "static inline double   array_get_double  (void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((double*)arr)[idx];   }\n");
    fprintf(out, "static inline void     array_set_double  (void* arr, size_t idx, double   val, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); ((double*)arr)[idx]   = val; }\n");
    fprintf(out, "static inline void* array_get_class(void* arr, size_t idx, const char* file, int line) { mylang_check_bounds(arr, idx, file, line); return ((void**)arr)[idx]; }\n");
    fprintf(out, "static inline void array_replace_class(void* arr, size_t idx, void* val, const char* file, int line) {\n");
    fprintf(out, "    void** _ap; void* _old;\n");
    fprintf(out, "    mylang_check_bounds(arr, idx, file, line);\n");
    fprintf(out, "    _ap = (void**)arr;\n");
    fprintf(out, "    _old = _ap[idx];\n");
    fprintf(out, "    _ap[idx] = val;\n");
    fprintf(out, "    mylang_release(_old);\n");
    fprintf(out, "}\n\n");
    fprintf(out, "static inline void* array_get_struct_ptr(void* arr, size_t idx, size_t elem_size, const char* file, int line) {\n");
    fprintf(out, "    mylang_check_bounds(arr, idx, file, line);\n");
    fprintf(out, "    return (char*)arr + idx * elem_size;\n");
    fprintf(out, "}\n\n");

    AstNode* decl = program->children[0];
    while (decl) {
        if (decl->kind == AST_STRUCT_DECL) {
            codegen_struct_decl(decl, out);
        }
        decl = decl->next;
    }

    decl = program->children[0];
    while (decl) {
        if (decl->kind == AST_CLASS_DECL) {
            codegen_class_decl(decl, out);
        }
        decl = decl->next;
    }

    decl = program->children[0];
    while (decl) {
        if (decl->kind == AST_FUNC_DECL) {
            codegen_func_decl(decl, out);
        }
        decl = decl->next;
    }
}
