#include "codegen.h"
#include "symtab.h"
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════�?   TYPE HELPERS
   ══════════════════════════════════════════════════════════════════�?*/

static const char* c_base_name(const Type* t) {
    switch (t->kind) {
        case TYPE_INT:    return "int";
        case TYPE_CHAR:   return "char";
        case TYPE_STRUCT: return t->struct_name;
        default:          return "int";
    }
}

static void c_type_str(const Type* t, char* buf, int bufsz) {
    if (t->is_pointer) {
        snprintf(buf, bufsz, "%s*", c_base_name(t));
    } else if (t->array_size > 0) {
        snprintf(buf, bufsz, "%s", c_base_name(t));
    } else {
        snprintf(buf, bufsz, "%s", c_base_name(t));
    }
}

/* ══════════════════════════════════════════════════════════════════�?   TYPE RESOLUTION (semantic analysis pass)
   ══════════════════════════════════════════════════════════════════�?*/

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
            StructInfo* si = symtab_find_struct(obj.struct_name);
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
            FuncInfo* fi = symtab_find_func(node->children[0]->tok.text);
            if (fi) { t = fi->return_type; }
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

/* ══════════════════════════════════════════════════════════════════�?   CODE GENERATION �?expressions
   ══════════════════════════════════════════════════════════════════�?*/

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
        fprintf(out, "calloc(1, sizeof(%s))", c_base_name(&base));
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
            fprintf(out, "%s", node->tok.text);
            break;
        case AST_BINARY:
            codegen_binary(node, out);
            break;
        case AST_UNARY:
            codegen_unary(node, out);
            break;
        case AST_ASSIGN:
            codegen_expr(node->children[0], out);
            fprintf(out, " = ");
            codegen_expr(node->children[1], out);
            break;
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

/* --- CODE GENERATION -- statements --- */

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
        fprintf(out, " = ");
        codegen_expr(node->children[0], out);
    }
    fprintf(out, ";\n");
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
        emit_bounds_checks(node->children[0], out, indent);
    }
    indent_line(out, indent);
    fprintf(out, "return");
    if (node->child_count > 0) {
        fprintf(out, " ");
        codegen_expr(node->children[0], out);
    }
    fprintf(out, ";\n");
}

static void codegen_expr_stmt(AstNode* node, FILE* out, int indent) {
    emit_bounds_checks(node->children[0], out, indent);
    indent_line(out, indent);
    codegen_expr(node->children[0], out);
    fprintf(out, ";\n");
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

/* ══════════════════════════════════════════════════════════════════�?   CODE GENERATION �?top-level declarations
   ══════════════════════════════════════════════════════════════════�?*/

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

    symtab_exit_scope();
    fprintf(out, "}\n\n");
}

/* ══════════════════════════════════════════════════════════════════�?   PUBLIC INTERFACE
   ══════════════════════════════════════════════════════════════════�?*/

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

    AstNode* decl = program->children[0];
    while (decl) {
        if (decl->kind == AST_STRUCT_DECL) {
            codegen_struct_decl(decl, out);
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
