#include "sema.h"
#include "util.h"
#include <string.h>

/* -------------------------------------------------------------------------
   Type resolution (moved from codegen.c; behavior unchanged, plus caching)
   ------------------------------------------------------------------------- */

MethodInfo* sema_static_call_method(AstNode* callee, ClassInfo** out_ci) {
    if (!callee || callee->ast_kind != AST_MEMBER_ACCESS) return NULL;
    AstNode* obj = callee->ast_children[0];
    if (!obj || obj->ast_kind != AST_IDENT) return NULL;
    if (symtab_lookup(obj->ast_token.text)) return NULL;
    ClassInfo* ci = symtab_find_class(obj->ast_token.text);
    if (!ci || ci->is_generic) return NULL;
    if (out_ci) *out_ci = ci;
    return symtab_find_method_in_class(ci, callee->ast_token.text);
}

Type sema_resolve_type(AstNode* node) {
    if (node->ast_is_resolved) return node->ast_resolved_type;

    Type t;
    memset(&t, 0, sizeof(t));

    switch (node->ast_kind) {
        case AST_INT_LIT:
            t.type_kind = TYPE_I32;
            t.type_id = TYPE_ID_I32;
            break;

        case AST_FLOAT_LIT: {
            int len = (int)strlen(node->ast_token.text);
            int is_f32 = (len > 0 && (node->ast_token.text[len - 1] == 'f' ||
                                       node->ast_token.text[len - 1] == 'F'));
            if (is_f32) {
                t.type_kind = TYPE_F32;
                t.type_id = TYPE_ID_F32;
            } else {
                t.type_kind = TYPE_F64;
                t.type_id = TYPE_ID_F64;
            }
            break;
        }

        case AST_CHAR_LIT:
            t.type_kind = TYPE_I8;
            t.type_id = TYPE_ID_I8;
            break;

        case AST_BOOL_LIT:
            t.type_kind = TYPE_BOOL;
            t.type_id = TYPE_ID_BOOL;
            break;

        case AST_NULL:
            t.type_kind = TYPE_NULL;
            break;

        case AST_STRING_LIT:
        case AST_FSTRING:
            t = type_make_user(TYPE_CLASS, "String");
            t.is_pointer = 1;
            t.type_id = TYPE_ID_STRING;
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
                t.type_kind = TYPE_BOOL;
                t.type_id = TYPE_ID_BOOL;
            } else {
                t = sema_resolve_type(node->ast_children[0]);
            }
            break;
        }

        case AST_UNARY:
            if (node->ast_token.kind == TOK_NOT) {
                sema_resolve_type(node->ast_children[0]);
                t.type_kind = TYPE_BOOL;
                t.type_id = TYPE_ID_BOOL;
            } else {
                t = sema_resolve_type(node->ast_children[0]);
            }
            break;

        case AST_INC_DEC:
            t = sema_resolve_type(node->ast_children[0]);
            break;

        case AST_ARRAY_ACCESS: {
            Type arr = sema_resolve_type(node->ast_children[0]);
            t = arr;
            /* An element is never itself an array.  Only class-reference elements
               (class or weak class) remain pointers; everything else is a value. */
            t.is_array = 0;
            t.array_size = 0;
            if (t.type_kind != TYPE_CLASS && t.type_kind != TYPE_OBJECT) {
                t.is_pointer = 0;
            }
            break;
        }

        case AST_MEMBER_ACCESS: {
            /* Enum variant access 'Key.Up': the left side is an enum name,
               not a variable.  A local variable of the same name shadows the
               enum (same rule as class static calls). */
            AstNode* obj_node = node->ast_children[0];
            if (obj_node->ast_kind == AST_IDENT &&
                !symtab_lookup(obj_node->ast_token.text)) {
                EnumInfo* ei = symtab_find_enum(obj_node->ast_token.text);
                if (ei) {
                    t = type_make_user(TYPE_ENUM, ei->name);
                    break;
                }
                /* Static class const access 'Config.MAX': the left side is a
                   class name, not a variable. */
                ClassInfo* cci = symtab_find_class(obj_node->ast_token.text);
                if (cci && !cci->is_generic) {
                    ConstInfo* cc = symtab_find_class_const(cci->name, node->ast_token.text);
                    if (cc) {
                        t = cc->const_type;
                        break;
                    }
                }
            }
            Type obj = sema_resolve_type(node->ast_children[0]);
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
                PropertyInfo* pi = symtab_find_property(ci, node->ast_token.text);
                if (pi) {
                    t = pi->prop_type;
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
                MethodInfo* smi = sema_static_call_method(mem, NULL);
                if (smi && smi->method_is_static) {
                    /* Static call via the class name: return type comes from
                       the method signature. */
                    t = smi->return_type;
                    break;
                }
                sema_resolve_type(obj);
                if (obj->ast_resolved_type.is_array &&
                    (strcmp(mem->ast_token.text, "length") == 0 ||
                     strcmp(mem->ast_token.text, "capacity") == 0)) {
                    /* Array length()/capacity() yield u64 (MyArray size_t members). */
                    t.type_kind = TYPE_U64;
                    t.type_id = TYPE_ID_U64;
                } else if (strcmp(mem->ast_token.text, "lock") == 0 && obj->ast_resolved_type.is_weak) {
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
                } else if (obj->ast_resolved_type.type_kind == TYPE_STRUCT) {
                    StructInfo* si = symtab_find_struct(obj->ast_resolved_type.class_name);
                    if (si) {
                        MethodInfo* mi = symtab_find_struct_method(si, mem->ast_token.text);
                        if (mi) { t = mi->return_type; }
                    }
                }
            } else {
                FuncInfo* fi = symtab_find_func(node->ast_children[0]->ast_token.text);
                if (fi) { t = fi->return_type; }
                else if (node->ast_children[0]->ast_kind == AST_IDENT &&
                         strcmp(node->ast_children[0]->ast_token.text, "hash") == 0) {
                    /* Builtin hash(x) yields u64. */
                    t.type_kind = TYPE_U64;
                    t.type_id = TYPE_ID_U64;
                } else if (node->ast_children[0]->ast_kind == AST_IDENT &&
                           strcmp(node->ast_children[0]->ast_token.text, "equals") == 0) {
                    /* Builtin equals(a, b) yields bool. */
                    t.type_kind = TYPE_BOOL;
                    t.type_id = TYPE_ID_BOOL;
                }
            }
            break;
        }

        case AST_NEW: {
            t = node->ast_resolved_type;
            t.is_pointer = 1;
            break;
        }

        case AST_ASSIGN:
            t = sema_resolve_type(node->ast_children[0]);
            break;

        case AST_VAR_DECL:
            t = node->ast_resolved_type;
            break;

        case AST_AS_CAST:
            sema_resolve_type(node->ast_children[0]);
            t = node->ast_resolved_type;
            break;

        case AST_REF_ARG:
            t = sema_resolve_type(node->ast_children[0]);
            break;

        default:
            break;
    }

    node->ast_resolved_type = t;
    node->ast_is_resolved = 1;
    return t;
}

/* -------------------------------------------------------------------------
   Forward pass: pre-resolve expression types in every reachable body
   ------------------------------------------------------------------------- */

/* Resolves every node of an expression tree (post-order).  Call argument
   lists are linked through 'next', so those are walked too. */
static void sema_walk_expr(AstNode* node) {
    if (!node) return;
    int i;
    for (i = 0; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
        sema_walk_expr(node->ast_children[i]);
    }
    sema_walk_expr(node->next);
    sema_resolve_type(node);
}

static void sema_walk_stmt(AstNode* node) {
    if (!node) return;

    switch (node->ast_kind) {
        case AST_BLOCK: {
            /* Blocks open no symbol scope (mirrors codegen_body). */
            AstNode* s = node->ast_children[0];
            while (s) {
                sema_walk_stmt(s);
                s = s->next;
            }
            break;
        }

        case AST_VAR_DECL:
            /* The initializer is resolved before the variable enters scope
               (mirrors codegen_var_decl). */
            if (node->ast_child_count > 0) {
                sema_walk_expr(node->ast_children[0]);
            }
            sema_resolve_type(node);
            symtab_insert(node->ast_token.text, node->ast_resolved_type);
            break;

        case AST_IF_STMT:
            sema_walk_expr(node->ast_children[0]);
            sema_walk_stmt(node->ast_children[1]);
            if (node->ast_child_count > 2) {
                sema_walk_stmt(node->ast_children[2]);
            }
            break;

        case AST_WHILE_STMT:
            sema_walk_expr(node->ast_children[0]);
            if (node->ast_child_count > 1) {
                sema_walk_stmt(node->ast_children[1]);
            }
            break;

        case AST_FOR_STMT:
            /* init / cond / step / body; the init variable stays in the
               enclosing scope (mirrors codegen_for_stmt). */
            if (node->ast_child_count > 0 && node->ast_children[0]) {
                if (node->ast_children[0]->ast_kind == AST_VAR_DECL) {
                    sema_walk_stmt(node->ast_children[0]);
                } else {
                    sema_walk_expr(node->ast_children[0]);
                }
            }
            if (node->ast_child_count > 1) sema_walk_expr(node->ast_children[1]);
            if (node->ast_child_count > 2) sema_walk_expr(node->ast_children[2]);
            if (node->ast_child_count > 3) sema_walk_stmt(node->ast_children[3]);
            break;

        case AST_FOREACH_STMT: {
            /* [decl, arr, body]; the loop variable is scoped to the loop
               (mirrors codegen_foreach_stmt). */
            AstNode* decl = (node->ast_child_count > 0) ? node->ast_children[0] : NULL;
            AstNode* arr  = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
            AstNode* body = (node->ast_child_count > 2) ? node->ast_children[2] : NULL;
            if (arr) sema_walk_expr(arr);
            symtab_enter_scope();
            if (decl) {
                sema_resolve_type(decl);
                symtab_insert(decl->ast_token.text, decl->ast_resolved_type);
            }
            sema_walk_stmt(body);
            symtab_exit_scope();
            break;
        }

        case AST_MATCH: {
            /* [expr, arms]; each arm body gets its own scope, and a class
               type pattern binds its variable there (mirrors
               codegen_match_stmt / codegen_match_arm_body). */
            if (node->ast_child_count > 0) sema_walk_expr(node->ast_children[0]);
            AstNode* arm = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
            while (arm) {
                symtab_enter_scope();
                if (arm->ast_resolved_type.type_kind == TYPE_CLASS) {
                    symtab_insert(arm->ast_token.text, arm->ast_resolved_type);
                }
                if (arm->ast_child_count > 0) {
                    sema_walk_stmt(arm->ast_children[0]);
                }
                symtab_exit_scope();
                arm = arm->next;
            }
            break;
        }

        case AST_RETURN_STMT:
            if (node->ast_child_count > 0) sema_walk_expr(node->ast_children[0]);
            break;

        case AST_EXPR_STMT:
            if (node->ast_child_count > 0) sema_walk_expr(node->ast_children[0]);
            break;

        case AST_BREAK:
        case AST_CONTINUE:
            break;

        default:
            /* Unknown statement shape: nothing is pre-resolved, codegen
               resolves on demand as before. */
            break;
    }
}

/* Walks a function/method body with the given parameters in scope, inserting
   'this' when insert_this is set (mirrors the three codegen emit sites). */
static void sema_walk_body(AstNode* params, AstNode* body,
                           int insert_this, Type* thiz_type) {
    symtab_enter_scope();
    if (insert_this) {
        symtab_insert("this", *thiz_type);
    }
    AstNode* p = params;
    while (p) {
        symtab_insert(p->ast_token.text, p->ast_resolved_type);
        p = p->next;
    }
    if (body && body->ast_kind == AST_BLOCK) {
        AstNode* s = body->ast_children[0];
        while (s) {
            sema_walk_stmt(s);
            s = s->next;
        }
    } else if (body) {
        sema_walk_stmt(body);
    }
    symtab_exit_scope();
}

static void sema_walk_method(AstNode* m, const char* owner_name, int is_struct) {
    AstNode* params = (m->ast_child_count == 2) ? m->ast_children[0] : NULL;
    AstNode* body   = (m->ast_child_count == 2) ? m->ast_children[1]
                                                : (m->ast_child_count == 1 ? m->ast_children[0] : NULL);
    Type thiz_type;
    memset(&thiz_type, 0, sizeof(thiz_type));
    int insert_this = 0;
    if (!m->ast_is_static) {
        thiz_type.type_kind = is_struct ? TYPE_STRUCT : TYPE_CLASS;
        CHECK_STRSCPY(strscpy(thiz_type.class_name, owner_name, sizeof(thiz_type.class_name)),
                      "owner type name too long");
        if (is_struct) {
            thiz_type.is_ref = 1;
        } else {
            thiz_type.is_pointer = 1;
        }
        insert_this = 1;
    }
    sema_walk_body(params, body, insert_this, &thiz_type);
}

void sema_check(AstNode* program) {
    if (!program || program->ast_kind != AST_PROGRAM) return;

    AstNode* decl = (program->ast_child_count > 0) ? program->ast_children[0] : NULL;
    while (decl) {
        if (decl->ast_kind == AST_CLASS_DECL) {
            ClassInfo* ci = symtab_find_class(decl->ast_token.text);
            /* Generic class templates are skipped: type parameters only
               resolve once codegen instantiates the class, which re-resolves
               the substituted nodes on demand (clones carry no cache). */
            if (ci && !ci->is_generic) {
                AstNode* m = (decl->ast_child_count > 0) ? decl->ast_children[0] : NULL;
                while (m) {
                    if (m->ast_kind == AST_FUNC_DECL) {
                        sema_walk_method(m, ci->name, 0);
                    }
                    m = m->next;
                }
            }
        } else if (decl->ast_kind == AST_STRUCT_DECL) {
            StructInfo* si = symtab_find_struct(decl->ast_token.text);
            if (si) {
                AstNode* m = (decl->ast_child_count > 0) ? decl->ast_children[0] : NULL;
                while (m) {
                    if (m->ast_kind == AST_FUNC_DECL) {
                        sema_walk_method(m, si->name, 1);
                    }
                    m = m->next;
                }
            }
        } else if (decl->ast_kind == AST_FUNC_DECL) {
            AstNode* params = (decl->ast_child_count == 2) ? decl->ast_children[0] : NULL;
            AstNode* body   = (decl->ast_child_count == 2) ? decl->ast_children[1]
                                                           : (decl->ast_child_count == 1 ? decl->ast_children[0] : NULL);
            sema_walk_body(params, body, 0, NULL);
        }
        decl = decl->next;
    }

    /* Interface default method bodies (mirrors emit_interface_default_methods:
       no 'this', parameters come from the interface registry). */
    {
        extern InterfaceInfo* interface_list;
        InterfaceInfo* ii = interface_list;
        while (ii) {
            int j;
            for (j = 0; j < ii->method_count; j++) {
                InterfaceMethodInfo* im = &ii->methods[j];
                AstNode* body = im->interface_method_default_body;
                if (!body) continue;
                symtab_enter_scope();
                int k;
                for (k = 0; k < im->param_count; k++) {
                    symtab_insert(im->param_names[k], im->param_types[k]);
                }
                if (body->ast_kind == AST_BLOCK) {
                    AstNode* s = body->ast_children[0];
                    while (s) {
                        sema_walk_stmt(s);
                        s = s->next;
                    }
                } else {
                    sema_walk_stmt(body);
                }
                symtab_exit_scope();
            }
            ii = ii->next;
        }
    }
}
