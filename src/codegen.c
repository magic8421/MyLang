#include "codegen.h"
#include "symtab.h"
#include <string.h>
#include <stdio.h>


static const char* c_base_name(const Type* t) {
    switch (t->kind) {
        case TYPE_INT:    return "int";
        case TYPE_CHAR:   return "char";
        case TYPE_CLASS: return t->class_name;
        default:          return "int";
    }
}

static void c_type_str(const Type* t, char* buf, int bufsz) {
    if (t->kind == TYPE_CLASS || t->is_pointer) {
        snprintf(buf, bufsz, "%s*", c_base_name(t));
    } else if (t->array_size > 0) {
        snprintf(buf, bufsz, "%s", c_base_name(t));
    } else {
        snprintf(buf, bufsz, "%s", c_base_name(t));
    }
}


static Type resolve_type(AstNode* node);

static Type resolve_type(AstNode* node) {
    Type t;
    memset(&t, 0, sizeof(t));

    switch (node->kind) {
        case AST_INT_LIT:
            t.kind = TYPE_INT;
            break;

        case AST_CHAR_LIT:
            t.kind = TYPE_CHAR;
            break;

        case AST_IDENT: {
            SymEntry* e = symtab_lookup(node->tok.text);
            if (e) { t = e->type; }
            break;
        }

        case AST_BINARY: {
            TokenKind op = node->tok.kind;
            if (op == TOK_EQ || op == TOK_NE ||
                op == TOK_LT || op == TOK_LE ||
                op == TOK_GT || op == TOK_GE ||
                op == TOK_AND || op == TOK_OR) {
                t.kind = TYPE_INT;
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
            t.is_pointer = 0;
            t.array_size = 0;
            break;
        }

        case AST_MEMBER_ACCESS: {
            Type obj = resolve_type(node->children[0]);
            ClassInfo* si = symtab_find_class(obj.class_name);
            if (si) {
                int i;
                for (i = 0; i < si->field_count; i++) {
                    if (strcmp(si->field_names[i], node->tok.text) == 0) {
                        t = si->field_types[i];
                        break;
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

static void codegen_call(AstNode* node, FILE* out) {
    /* method call: p.foo(...) ClassName_foo(p, ...) */
    if (node->children[0]->kind == AST_MEMBER_ACCESS) {
        AstNode* mem = node->children[0];
        AstNode* obj = mem->children[0];
        const char* mname = mem->tok.text;
        resolve_type(obj);
        if (obj->resolved_type.kind == TYPE_CLASS) {
            fprintf(out, "%s_%s(", obj->resolved_type.class_name, mname);
            codegen_expr(obj, out);
            AstNode* args = (node->child_count > 1) ? node->children[1] : NULL;
            while (args) {
                fprintf(out, ", ");
                codegen_expr(args, out);
                args = args->next;
            }
            fprintf(out, ")");
            return;
        }
    }
    /* normal function call */
    codegen_expr(node->children[0], out);
    fprintf(out, "(");
    AstNode* args = (node->child_count > 1) ? node->children[1] : NULL;
    int first = 1;
    while (args) {
        if (!first) fprintf(out, ", ");
        codegen_expr(args, out);
        first = 0;
        args = args->next;
    }
    fprintf(out, ")");
}

static void codegen_array_access(AstNode* node, FILE* out) {
    codegen_expr(node->children[0], out);
    fprintf(out, "[");
    codegen_expr(node->children[1], out);
    fprintf(out, "]");
}

static void codegen_member_access(AstNode* node, FILE* out) {
    AstNode* obj = node->children[0];
    codegen_expr(obj, out);
    if (obj->resolved_type.is_pointer) {
        fprintf(out, "->");
    } else {
        fprintf(out, ".");
    }
    fprintf(out, "%s", node->tok.text);
}

static void codegen_new(AstNode* node, FILE* out) {
    Type base = node->resolved_type;
    if (node->child_count > 0) {
        fprintf(out, "mylang_new_array(");
        codegen_expr(node->children[0], out);
        fprintf(out, ", sizeof(");
        fprintf(out, "%s", c_base_name(&base));
        fprintf(out, "))");
    } else {
        if (base.kind == TYPE_CLASS) {
            fprintf(out, "mylang_new_object(sizeof(%s))", c_base_name(&base));
        } else {
            fprintf(out, "calloc(1, sizeof(%s))", c_base_name(&base));
        }
    }
    base.is_pointer = 1;
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

    switch (node->kind) {
        case AST_INT_LIT:
            fprintf(out, "%d", node->tok.int_val);
            break;
        case AST_CHAR_LIT:
            codegen_char_lit(node, out);
            break;
        case AST_IDENT:
            if (strcmp(node->tok.text, "this") == 0)
                fprintf(out, "thiz");
            else
                fprintf(out, "%s", node->tok.text);
            break;
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

            if (lhs->kind == AST_IDENT && lt.kind == TYPE_CLASS && lt.is_pointer) {
                int rhs_simple = (rhs->kind == AST_IDENT || rhs->kind == AST_INT_LIT || rhs->kind == AST_CHAR_LIT);

                if (rhs->kind == AST_NEW) {
                    fprintf(out, "((void)mylang_release(");
                    codegen_expr(lhs, out);
                    fprintf(out, "), (");
                    codegen_expr(lhs, out);
                    fprintf(out, " = ");
                    codegen_expr(rhs, out);
                    fprintf(out, "))");
                } else if (rhs->kind == AST_CALL) {
                    fprintf(out, "((void)mylang_release(");
                    codegen_expr(lhs, out);
                    fprintf(out, "), (");
                    codegen_expr(lhs, out);
                    fprintf(out, " = ");
                    codegen_expr(rhs, out);
                    fprintf(out, "))");
                } else if (rhs_simple) {
                    fprintf(out, "((void)mylang_retain(");
                    codegen_expr(rhs, out);
                    fprintf(out, "), (void)mylang_release(");
                    codegen_expr(lhs, out);
                    fprintf(out, "), (");
                    codegen_expr(lhs, out);
                    fprintf(out, " = ");
                    codegen_expr(rhs, out);
                    fprintf(out, "))");
                } else {
                    fprintf(out, "((void)mylang_release(");
                    codegen_expr(lhs, out);
                    fprintf(out, "), (");
                    codegen_expr(lhs, out);
                    fprintf(out, " = mylang_retain(");
                    codegen_expr(rhs, out);
                    fprintf(out, ")))");
                }
            } else {
                codegen_expr(node->children[0], out);
                fprintf(out, " = ");
                codegen_expr(node->children[1], out);
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

        if (at.array_size > 0) {
            indent_line(out, indent);
            fprintf(out, "if ((size_t)(");
            codegen_expr(idx, out);
            fprintf(out, ") >= %d) __debugbreak();\n", at.array_size);
        } else if (at.is_pointer && (at.kind == TYPE_INT || at.kind == TYPE_CHAR)) {
            indent_line(out, indent);
            fprintf(out, "mylang_bounds(");
            codegen_expr(arr, out);
            fprintf(out, ", ");
            codegen_expr(idx, out);
            fprintf(out, ");\n");
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

static int call_needs_guard(AstNode* arg) {
    resolve_type(arg);
    if (arg->resolved_type.kind != TYPE_CLASS) return 0;
    if (arg->kind == AST_IDENT && symtab_lookup(arg->tok.text)) return 0;
    return 1;
}

static void emit_call_guards(AstNode* expr, FILE* out, int is_retain) {
    if (!expr) return;
    if (expr->kind == AST_CALL) {
        AstNode* callee = expr->children[0];
        AstNode* args  = (expr->child_count > 1) ? expr->children[1] : NULL;

        if (callee->kind == AST_MEMBER_ACCESS) {
            AstNode* obj = callee->children[0];
            if (call_needs_guard(obj)) {
                if (is_retain) { fprintf(out, "mylang_retain("); codegen_expr(obj, out); fprintf(out, "); "); }
                else           { fprintf(out, "mylang_release("); codegen_expr(obj, out); fprintf(out, "); "); }
            }
        }
        AstNode* a = args;
        while (a) {
            if (call_needs_guard(a)) {
                if (is_retain) { fprintf(out, "mylang_retain("); codegen_expr(a, out); fprintf(out, "); "); }
                else           { fprintf(out, "mylang_release("); codegen_expr(a, out); fprintf(out, "); "); }
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
static const char* cleanup_names[MAX_CLEANUP];
static int         cleanup_count = 0;
static int         assign_tmp_id = 0;

static void cleanup_add(const char* name) {
    if (cleanup_count < MAX_CLEANUP) cleanup_names[cleanup_count++] = name;
}

static void cleanup_emit(FILE* out, int indent) {
    int i;
    for (i = cleanup_count - 1; i >= 0; i--) {
        indent_line(out, indent);
        fprintf(out, "mylang_release(%s);\n",
                strcmp(cleanup_names[i], "this") == 0 ? "thiz" : cleanup_names[i]);
    }
}

static void cleanup_remove(const char* name) {
    int i;
    for (i = 0; i < cleanup_count; i++) {
        if (strcmp(cleanup_names[i], name) == 0) {
            int j;
            for (j = i; j < cleanup_count - 1; j++) cleanup_names[j] = cleanup_names[j + 1];
            cleanup_count--;
            return;
        }
    }
}

static void cleanup_clear(void) { cleanup_count = 0; }

static void cleanup_remove_from_expr(AstNode* expr) {
    if (!expr) return;
    if (expr->kind == AST_IDENT) {
        resolve_type(expr);
        if (expr->resolved_type.kind == TYPE_CLASS) {
            cleanup_remove(expr->tok.text);
        }
    }
    int i;
    for (i = 0; i < expr->child_count; i++) {
        cleanup_remove_from_expr(expr->children[i]);
    }
    cleanup_remove_from_expr(expr->next);
}

static void codegen_stmt(AstNode* node, FILE* out, int indent);

static void indent_line(FILE* out, int indent) {
    int i;
    for (i = 0; i < indent; i++) fprintf(out, "    ");
}

static void codegen_body(AstNode* body, FILE* out, int indent) {
    indent_line(out, indent);
    fprintf(out, "{\n");
    if (body->kind == AST_BLOCK) {
        AstNode* s = body->children[0];
        while (s) {
            codegen_stmt(s, out, indent + 1);
            s = s->next;
        }
    } else {
        codegen_stmt(body, out, indent + 1);
    }
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
    } else if (type.kind == TYPE_CLASS && type.is_pointer) {
        fprintf(out, " = NULL");
    }
    fprintf(out, ";\n");

    if (type.kind == TYPE_CLASS && type.is_pointer) {
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
            cleanup_clear();
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
            cleanup_clear();
            indent_line(out, indent);
            fprintf(out, "return _mylang_ret;\n");
        }
    } else {
        cleanup_emit(out, indent);
        cleanup_clear();
        indent_line(out, indent);
        fprintf(out, "return;\n");
    }
}

static void codegen_expr_stmt(AstNode* node, FILE* out, int indent) {
    emit_bounds_checks(node->children[0], out, indent);
    AstNode* expr = node->children[0];
    resolve_type(expr);

    /* class pointer assignment: evaluate RHS first, release old LHS, then assign.
       This avoids use-after-free when RHS aliases LHS (e.g. b = b.set(5)). */
    if (expr->kind == AST_ASSIGN) {
        AstNode* lhs = expr->children[0];
        AstNode* rhs = expr->children[1];
        resolve_type(lhs);
        resolve_type(rhs);
        Type lt = lhs->resolved_type;
        if (lhs->kind == AST_IDENT && lt.kind == TYPE_CLASS && lt.is_pointer) {
            emit_stmt_call_retains(expr, out, indent);

            int id = assign_tmp_id++;
            indent_line(out, indent);
            fprintf(out, "void* _my_assign_%d = ", id);
            if (rhs->kind == AST_CALL || rhs->kind == AST_NEW) {
                codegen_expr(rhs, out);
            } else {
                fprintf(out, "mylang_retain(");
                codegen_expr(rhs, out);
                fprintf(out, ")");
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
        fprintf(out, "%s %s", pt, p->tok.text); p = p->next; } }
    fprintf(out, ")\n{\n");
    cleanup_clear(); symtab_enter_scope();
    Type thiz_type; memset(&thiz_type, 0, sizeof(thiz_type));
    thiz_type.kind = TYPE_CLASS; strncpy(thiz_type.class_name, class_name, 63);
    thiz_type.is_pointer = 1; symtab_insert("this", thiz_type);
    { AstNode* p = params; while (p) { symtab_insert(p->tok.text, p->resolved_type);
        p = p->next; } }
    if (body && body->kind == AST_BLOCK) { AstNode* s = body->children[0];
        while (s) { codegen_stmt(s, out, 1); s = s->next; } }
    cleanup_emit(out, 1);
    symtab_exit_scope(); fprintf(out, "}\n\n");
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
            fprintf(out, "%s %s", ptype_buf, p->tok.text);
            first = 0;
            p = p->next;
        }
    }
    fprintf(out, ")\n");

    /* body */
    cleanup_clear();
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

    /* walk body statements */
    if (body && body->kind == AST_BLOCK) {
        AstNode* s = body->children[0];
        while (s) {
            codegen_stmt(s, out, 1);
            s = s->next;
        }
    }

    cleanup_emit(out, 1);
    symtab_exit_scope();
    fprintf(out, "}\n\n");
}

void codegen_program(AstNode* program, FILE* out) {
    fprintf(out, "/* Generated by MyLang compiler */\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stddef.h>\n");
    fprintf(out, "#ifdef _MSC_VER\n");
    fprintf(out, "#include <intrin.h>\n");
    fprintf(out, "#else\n");
    fprintf(out, "#define __debugbreak() __builtin_trap()\n");
    fprintf(out, "#endif\n\n");

    fprintf(out, "static void* mylang_new_array(size_t count, size_t elem_size) {\n");
    fprintf(out, "    size_t* p = calloc(1, sizeof(size_t) + count * elem_size);\n");
    fprintf(out, "    p[0] = count;\n");
    fprintf(out, "    return p + 1;\n");
    fprintf(out, "}\n\n");

    fprintf(out, "#define mylang_bounds(arr, idx) do { \\\n");
    fprintf(out, "    if ((size_t)(idx) >= *(size_t*)((char*)(arr) - sizeof(size_t))) \\\n");
    fprintf(out, "        __debugbreak(); \\\n");
    fprintf(out, "} while(0)\n\n");

    fprintf(out, "#ifdef _MSC_VER\n");
    fprintf(out, "#include <windows.h>\n");
    fprintf(out, "#define mylang_atomic_inc(p) InterlockedIncrement(p)\n");
    fprintf(out, "#define mylang_atomic_dec(p) InterlockedDecrement(p)\n");
    fprintf(out, "#else\n");
    fprintf(out, "#include <stdatomic.h>\n");
    fprintf(out, "#define mylang_atomic_inc(p) (atomic_fetch_add(p, 1) + 1)\n");
    fprintf(out, "#define mylang_atomic_dec(p) (atomic_fetch_sub(p, 1) - 1)\n");
    fprintf(out, "#endif\n\n");

    fprintf(out, "typedef struct { volatile long refcount; } ObjHeader;\n");
    fprintf(out, "#define mylang_obj_hdr(ptr) ((ObjHeader*)((char*)(ptr) - sizeof(ObjHeader)))\n\n");

    fprintf(out, "static void* mylang_new_object(size_t sz) {\n");
    fprintf(out, "    ObjHeader* h = calloc(1, sizeof(ObjHeader) + sz);\n");
    fprintf(out, "    h->refcount = 1;\n");
    fprintf(out, "    return h + 1;\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static void* mylang_retain(void* ptr) {\n");
    fprintf(out, "    if (ptr) mylang_atomic_inc(&mylang_obj_hdr(ptr)->refcount);\n");
    fprintf(out, "    return ptr;\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static int mylang_release(void* ptr) {\n");
    fprintf(out, "    if (ptr && mylang_atomic_dec(&mylang_obj_hdr(ptr)->refcount) == 0) {\n");
    fprintf(out, "        free(mylang_obj_hdr(ptr));\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n\n");

    AstNode* decl = program->children[0];
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
