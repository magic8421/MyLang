#include "sema.h"
#include "util.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------
   Diagnostics (VS Code-compatible location prefix, same shape as codegen's)
   ------------------------------------------------------------------------- */

static int s_sema_error = 0;

int sema_had_error(void) { return s_sema_error; }

/* Reports an error located at the given node: "path(line,col): error: msg".
   The message text is identical to the codegen diagnostic it replaces; the
   negative tests match on it. */
static void sema_report_error(AstNode* node, const char* fmt, ...) {
    const char* file = (node && node->ast_token.filename && node->ast_token.filename[0])
                       ? node->ast_token.filename : "<unknown>";
    int line = node ? node->ast_token.line : 0;
    int col  = node ? node->ast_token.col : 0;
    va_list ap;
    fprintf(stderr, "%s(%d,%d): error: ", file, line, col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    s_sema_error = 1;
}

/* -------------------------------------------------------------------------
   Predicates shared with codegen (moved from codegen.c, behavior unchanged)
   ------------------------------------------------------------------------- */

int is_bit_compound_op(TokenKind k) {
    return k == TOK_AMP_ASSIGN || k == TOK_PIPE_ASSIGN || k == TOK_CARET_ASSIGN ||
           k == TOK_SHL_ASSIGN || k == TOK_SHR_ASSIGN;
}

int is_compound_assign_op(TokenKind k) {
    return k == TOK_PLUS_ASSIGN || k == TOK_MINUS_ASSIGN ||
           k == TOK_STAR_ASSIGN || k == TOK_SLASH_ASSIGN ||
           k == TOK_AMP_ASSIGN || k == TOK_PIPE_ASSIGN || k == TOK_CARET_ASSIGN ||
           k == TOK_SHL_ASSIGN || k == TOK_SHR_ASSIGN;
}

PropertyInfo* member_access_property(AstNode* node, ClassInfo** out_ci) {
    if (!node || node->ast_kind != AST_MEMBER_ACCESS) return NULL;
    AstNode* obj = node->ast_children[0];
    if (!obj) return NULL;
    sema_resolve_type(obj);
    if (obj->ast_resolved_type.type_kind != TYPE_CLASS) return NULL;
    ClassInfo* ci = symtab_find_class(obj->ast_resolved_type.class_name);
    if (!ci || ci->is_generic) return NULL;
    PropertyInfo* pi = symtab_find_property(ci, node->ast_token.text);
    if (pi && out_ci) *out_ci = ci;
    return pi;
}

/* Interface-implementation predicate (moved from codegen.c, behavior
   unchanged). */
int class_implements(ClassInfo* ci, const char* iname) {
    if (!ci) return 0;
    int i;
    for (i = 0; i < ci->impl_count && i < MAX_IMPL; i++) {
        if (strcmp(ci->impl_names[i], iname) == 0) return 1;
    }
    return 0;
}

/* Pass-by-reference predicate (moved from codegen.c, behavior unchanged). */
int type_is_ref(const Type* t) {
    return t->is_ref;
}

/* Class lookup that also materialises generic instantiations (moved from
   codegen.c, behavior unchanged). */
ClassInfo* class_info_for_type(Type* t) {
    if (t->type_arg_count > 0) return symtab_instantiate_class_from_type(t);
    return symtab_find_class(t->class_name);
}

/* Element type of a MyArray value type (moved from codegen.c, behavior
   unchanged). */
Type array_elem_type(const Type* arr_type) {
    Type et = *arr_type;
    et.is_array = 0;
    et.array_size = 0;
    return et;
}

/* True when the expression is a lock() call on an unowned reference, which
   yields a strong value that may be assigned to a strong class variable.
   The codegen_call path emits a dedicated error message for this; we must not
   shadow it with a generic type-mismatch error in the variable-init or
   assignment paths.  Callers have already resolved the source expression. */
int expr_is_unowned_lock(AstNode* node) {
    if (!node || node->ast_kind != AST_CALL || node->ast_child_count < 1) return 0;
    AstNode* mem = node->ast_children[0];
    if (!mem || mem->ast_kind != AST_MEMBER_ACCESS) return 0;
    if (strcmp(mem->ast_token.text, "lock") != 0) return 0;
    AstNode* obj = mem->ast_children[0];
    if (!obj) return 0;
    return obj->ast_resolved_type.is_unowned;
}

int struct_has_ref_fields(const Type* t) {
    if (t->type_kind != TYPE_STRUCT) return 0;
    StructInfo* si = symtab_find_struct(t->class_name);
    return si && si->has_ref_fields;
}

int type_is_ref_struct_array(const Type* t) {
    if (!t->is_array) return 0;
    Type et = *t;
    et.is_array = 0;
    et.array_size = 0;
    return struct_has_ref_fields(&et);
}

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

/* Assignment type checks, migrated from the codegen AST_ASSIGN dispatch.
   The branch chain mirrors the dispatch exactly (array/null/const guards,
   compound ops, array elements, then the lhs-kind chain); codegen keeps its
   copies as the fallback for generic instantiations, which sema never sees.
   Property writes are skipped: they are still unlowered at sema time and keep
   their dedicated codegen diagnostics. */
static void sema_check_assign(AstNode* node) {
    AstNode* lhs = node->ast_children[0];
    AstNode* rhs = node->ast_children[1];
    if (!lhs || !rhs) return;
    Type lt = lhs->ast_resolved_type;
    Type rt = rhs->ast_resolved_type;

    if (lt.is_array) {
        sema_report_error(lhs, "cannot assign arrays directly; use move_to(ref) or copy_to(ref)");
        return;
    }
    if (type_is_null(&lt)) {
        sema_report_error(node, "cannot assign to null literal");
        return;
    }
    if (lt.is_const) {
        sema_report_error(node, "cannot assign to const variable '%s'", lhs->ast_token.text);
        return;
    }
    if (member_access_property(lhs, NULL)) return;

    TokenKind assign_op = node->ast_token.kind;
    if (is_compound_assign_op(assign_op)) {
        /* Compound assignment: arithmetic ops accept primitive numeric
           types; bitwise ops require integer types. */
        int type_ok = is_bit_compound_op(assign_op)
            ? type_is_integer(&lt) : type_is_numeric(&lt);
        if (!type_ok) {
            sema_report_error(node, "compound assignment not supported for this type");
        }
        return;
    }

    if (lhs->ast_kind == AST_ARRAY_ACCESS && lhs->ast_children[0] &&
        lhs->ast_children[0]->ast_resolved_type.is_array) {
        /* Array element assignment. */
        if (lt.type_kind == TYPE_OBJECT) {
            int rhs_iface = (rt.type_kind == TYPE_INTERFACE && !rt.is_weak);
            if (!rhs_iface && rt.type_kind != TYPE_CLASS &&
                rt.type_kind != TYPE_OBJECT && !type_is_null(&rt)) {
                sema_report_error(node, "cannot assign '%s' to 'object' array element", type_name(&rt));
            }
            return;
        }
        if (type_is_reference(&lt) || lt.is_weak) return;
        /* primitive/struct/bool element: same checks as the plain branch */
        if (type_is_null(&rt)) {
            sema_report_error(node, "cannot assign 'null' to '%s'", type_name(&lt));
        } else if (bool_mismatch(&lt, &rt) || enum_mismatch(&lt, &rt)) {
            sema_report_error(node, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
        } else if (type_is_reference(&rt)) {
            sema_report_error(node, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
        }
        return;
    }

    if ((lt.is_weak || lt.is_unowned) && lt.type_kind == TYPE_CLASS) {
        /* Weak/unowned class variable or field. */
        if (type_is_null(&rt) && lt.is_unowned) {
            sema_report_error(node, "cannot assign null to unowned reference");
            return;
        }
        if (rt.type_kind != TYPE_CLASS && !type_is_null(&rt) && !rt.is_weak && !rt.is_unowned) {
            sema_report_error(node, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
        }
        return;
    }
    if (lt.type_kind == TYPE_CLASS) {
        if (rt.type_kind == TYPE_OBJECT) {
            sema_report_error(node, "cannot assign 'object' to '%s'; cast with 'as' first", type_name(&lt));
            return;
        }
        if (rt.type_kind != TYPE_CLASS && !type_is_null(&rt) && !expr_is_unowned_lock(rhs)) {
            sema_report_error(node, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
            return;
        }
        if (rt.is_weak) {
            sema_report_error(node, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
        }
        return;
    }
    if (lt.type_kind == TYPE_OBJECT) {
        int rhs_iface = (rt.type_kind == TYPE_INTERFACE && !rt.is_weak);
        if (!rhs_iface && rt.type_kind != TYPE_CLASS &&
            rt.type_kind != TYPE_OBJECT && !type_is_null(&rt)) {
            sema_report_error(node, "cannot assign '%s' to 'object'", type_name(&rt));
        }
        return;
    }
    if (lt.is_weak && lt.type_kind == TYPE_INTERFACE) {
        if (!(rt.is_weak && rt.type_kind == TYPE_INTERFACE) &&
            rt.type_kind != TYPE_INTERFACE && rt.type_kind != TYPE_CLASS &&
            !type_is_null(&rt)) {
            sema_report_error(node, "cannot assign to weak interface from this type");
        }
        return;
    }
    if (lt.type_kind == TYPE_INTERFACE) {
        /* No dedicated checks in the assign dispatch. */
        return;
    }

    /* primitive/struct/bool plain assignment */
    if (type_is_null(&rt)) {
        sema_report_error(node, "cannot assign 'null' to '%s'", type_name(&lt));
    } else if (bool_mismatch(&lt, &rt) || enum_mismatch(&lt, &rt)) {
        sema_report_error(node, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
    } else if (type_is_reference(&rt)) {
        sema_report_error(node, "cannot assign '%s' to '%s'", type_name(&rt), type_name(&lt));
    }
}

/* Variable-initializer type checks, migrated from codegen_var_decl.  The
   branch chain mirrors it exactly (null guard, array, weak interface,
   unowned, weak/unowned with init, interface, const, then the generic tail);
   codegen keeps its copies as the fallback for generic instantiations. */
static void sema_check_var_decl(AstNode* node) {
    Type type = node->ast_resolved_type;
    AstNode* init = (node->ast_child_count > 0) ? node->ast_children[0] : NULL;
    Type it;
    memset(&it, 0, sizeof(it));
    if (init) it = init->ast_resolved_type;

    /* null may only initialize reference types (class, interface, weak). */
    if (init && type_is_null(&it) && !type_accepts_null(&type)) {
        if (type.is_unowned) {
            sema_report_error(node, "cannot initialize unowned '%s' with 'null'", type.class_name);
        } else {
            sema_report_error(node, "cannot initialize '%s' with 'null'", type_name(&type));
        }
        return;
    }

    if (type.is_array) {
        if (type_is_ref_struct_array(&type)) {
            sema_report_error(node, "arrays of struct '%s' with reference fields are not supported yet", type.class_name);
        }
        return;
    }

    if (type.is_weak && type.type_kind == TYPE_INTERFACE) {
        if (init && !(it.is_weak && it.type_kind == TYPE_INTERFACE) &&
            it.type_kind != TYPE_INTERFACE && it.type_kind != TYPE_CLASS &&
            !type_is_null(&it)) {
            sema_report_error(node, "cannot initialize weak interface '%s' with this value", type.class_name);
        }
        return;
    }

    if (type.is_unowned && !init) {
        sema_report_error(node, "unowned variable '%s' requires an initializer", node->ast_token.text);
        return;
    }

    if ((type.is_weak || type.is_unowned) && init) {
        /* null with unowned was already rejected by the guard above. */
        if (it.type_kind != TYPE_CLASS && !type_is_null(&it)) {
            sema_report_error(node, "cannot initialize '%s' with '%s'", type_name(&type), type_name(&it));
        }
        return;
    }

    if (type.type_kind == TYPE_INTERFACE) {
        if (init && it.type_kind != TYPE_CLASS && it.type_kind != TYPE_INTERFACE &&
            !type_is_null(&it)) {
            sema_report_error(node, "cannot initialize interface '%s' with non-class value", type.class_name);
        }
        return;
    }

    if (type.is_const && !init) {
        sema_report_error(node, "const variable '%s' requires an initializer", node->ast_token.text);
        return;
    }

    if (!init) return;

    if (type_is_null(&it)) {
        /* Only reference types reach here with a null initializer. */
        return;
    }
    if (type.type_kind == TYPE_OBJECT) {
        /* object accepts any class, interface, or object value. */
        if (!(it.type_kind == TYPE_INTERFACE && !it.is_weak) &&
            it.type_kind != TYPE_CLASS && it.type_kind != TYPE_OBJECT) {
            sema_report_error(node, "cannot initialize 'object' with '%s'", type_name(&it));
        }
        return;
    }
    if (type.type_kind == TYPE_CLASS && it.type_kind == TYPE_OBJECT) {
        sema_report_error(node, "cannot initialize '%s' with 'object'; cast with 'as' first", type_name(&type));
        return;
    }
    if (type.type_kind == TYPE_CLASS) {
        if (!expr_is_unowned_lock(init) && (it.type_kind != TYPE_CLASS || it.is_weak)) {
            sema_report_error(node, "cannot initialize '%s' with '%s'", type_name(&type), type_name(&it));
        }
        return;
    }

    /* primitive/struct/bool/enum targets */
    if (bool_mismatch(&type, &it) || enum_mismatch(&type, &it)) {
        sema_report_error(node, "cannot initialize '%s' with '%s'", type_name(&type), type_name(&it));
    } else if (type_is_reference(&it)) {
        sema_report_error(node, "cannot initialize '%s' with '%s'", type_name(&type), type_name(&it));
    }
}

/* Binary/unary operator operand checks, migrated from codegen_binary and
   codegen_unary.  The check order mirrors codegen exactly (null, reference
   comparison, enum, bitwise); codegen keeps its copies as the fallback for
   generic instantiations.  Reporting these in sema also keeps downstream
   cascade errors (e.g. an initializer mismatch on the result type) behind
   the primary operator diagnostic. */
static void sema_check_binary(AstNode* node) {
    TokenKind op = node->ast_token.kind;
    Type lt = node->ast_children[0]->ast_resolved_type;
    Type rt = node->ast_children[1]->ast_resolved_type;

    if (type_is_null(&lt) || type_is_null(&rt)) {
        /* null may only be compared for (in)equality. */
        if (op != TOK_EQ && op != TOK_NE) {
            sema_report_error(node, "operator '%s' not allowed with null", node->ast_token.text);
            return;
        }
        Type nt = type_is_null(&lt) ? rt : lt;
        if (nt.type_kind != TYPE_INTERFACE && nt.type_kind != TYPE_CLASS &&
            nt.type_kind != TYPE_OBJECT && !type_is_null(&nt)) {
            sema_report_error(node, "cannot compare '%s' with null", type_name(&nt));
        }
        return;
    }

    /* Reference-like types may only be compared with other reference-like
       types (or null, handled above). */
    if (op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_LE ||
        op == TOK_GT || op == TOK_GE) {
        int lhs_ref = type_is_reference(&lt);
        int rhs_ref = type_is_reference(&rt);
        if ((lhs_ref || rhs_ref) && !(lhs_ref && rhs_ref)) {
            sema_report_error(node, "cannot compare '%s' with '%s'", type_name(&lt), type_name(&rt));
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
            sema_report_error(node, "operator '%s' not allowed for operands of type '%s' and '%s'",
                    node->ast_token.text, type_name(&lt), type_name(&rt));
            return;
        }
    }

    if (op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET ||
        op == TOK_SHL || op == TOK_SHR) {
        /* Bitwise operators accept integer operands only. */
        if (!type_is_integer(&lt) || !type_is_integer(&rt)) {
            sema_report_error(node, "operator '%s' requires integer operands", node->ast_token.text);
        }
    }
}

static void sema_check_unary(AstNode* node) {
    if (node->ast_token.kind == TOK_TILDE) {
        Type t = node->ast_children[0]->ast_resolved_type;
        if (!type_is_integer(&t)) {
            sema_report_error(node, "operator '~' requires an integer operand");
        }
    }
}

/* Return type of the function/method body currently being walked; set by
   sema_walk_body and the interface-default-method loop (mirrors
   ctx->return_type in codegen). */
static Type s_current_ret_type;

/* Mirrors the diagnostics of codegen_return_stmt.  The expression has
   already been resolved (and checked) by sema_walk_expr. */
static void sema_check_return(AstNode* node) {
    if (node->ast_child_count == 0) return;  /* bare 'return': no diagnostic */
    AstNode* ret = node->ast_children[0];

    if (type_is_null(&ret->ast_resolved_type)) {
        /* return null: only reference return types are allowed. */
        if (!type_accepts_null(&s_current_ret_type)) {
            sema_report_error(ret, "cannot return null from function returning '%s'",
                              type_name(&s_current_ret_type));
        }
        return;
    }

    /* Strict bool/enum rules at the return boundary. */
    if (s_current_ret_type.type_kind != TYPE_VOID &&
        (bool_mismatch(&s_current_ret_type, &ret->ast_resolved_type) ||
         enum_mismatch(&s_current_ret_type, &ret->ast_resolved_type))) {
        sema_report_error(ret, "cannot return '%s' from function returning '%s'",
                          type_name(&ret->ast_resolved_type), type_name(&s_current_ret_type));
    }

    /* object converts back to a concrete type only through 'as'. */
    if (ret->ast_resolved_type.type_kind == TYPE_OBJECT &&
        s_current_ret_type.type_kind != TYPE_OBJECT) {
        sema_report_error(ret, "cannot return 'object' from function returning '%s'; cast with 'as' first",
                          type_name(&s_current_ret_type));
    }

    /* Array-by-value: mirrors the emission chain in codegen_return_stmt,
       where class/object/interface values take their own branches first. */
    TypeKind k = ret->ast_resolved_type.type_kind;
    if (k != TYPE_CLASS && k != TYPE_OBJECT && k != TYPE_INTERFACE &&
        ret->ast_resolved_type.is_array) {
        sema_report_error(ret, "cannot return array by value; use move_to(ref) through a ref parameter");
    }
}

/* -------------------------------------------------------------------------
   Call boundary checks (mirrors codegen_call / codegen_call_arg)
   ------------------------------------------------------------------------- */

/* Array builtin method names (moved from codegen.c): these have fixed
   signatures and skip the user-call boundary checks. */
int is_array_method_name(const char* s) {
    return strcmp(s, "push") == 0 || strcmp(s, "pop") == 0 ||
           strcmp(s, "reserve") == 0 || strcmp(s, "resize") == 0 ||
           strcmp(s, "clear") == 0 || strcmp(s, "compact") == 0 ||
           strcmp(s, "length") == 0 || strcmp(s, "capacity") == 0 ||
           strcmp(s, "move_to") == 0 || strcmp(s, "copy_to") == 0;
}

/* Counts the arguments of a call node (the children[1] next-chain). */
static int sema_count_call_args(AstNode* node) {
    int count = 0;
    AstNode* a = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
    while (a) {
        count++;
        a = a->next;
    }
    return count;
}

/* Mirrors codegen_check_call_arity.  Default values make trailing parameters
   optional, so 'required' counts the leading parameters without one.  Runs on
   the raw (pre-default-filling) argument list, which yields the same verdict
   and numbers as codegen's post-filling count: filled defaults only ever
   extend the list up to param_count, and only when every filled parameter
   has a default. */
static void sema_check_call_arity(AstNode* at, const char* display_name,
                                  int actual, int param_count,
                                  AstNode* const* param_defaults) {
    int required = 0;
    while (required < param_count && !param_defaults[required]) required++;
    if (actual < required) {
        sema_report_error(at, "too few arguments for '%s' (expected at least %d, got %d)",
                          display_name, required, actual);
    } else if (actual > param_count) {
        sema_report_error(at, "too many arguments for '%s' (expected %d, got %d)",
                          display_name, param_count, actual);
    }
}

/* Mirrors the diagnostics of codegen_call_arg for a single argument.  A
   zeroed param_type (unknown signature) only rejects REF_ARG arguments,
   exactly like codegen. */
static void sema_check_call_arg(AstNode* arg, const Type* param_type) {
    if (param_type->is_array && !param_type->is_ref) {
        sema_report_error(arg, "array arguments must be passed by ref");
        return;
    }
    if (param_type->is_ref) {
        if (arg->ast_kind != AST_REF_ARG) {
            sema_report_error(arg, "missing 'ref' keyword for ref parameter");
            return;
        }
        AstNode* var = arg->ast_children[0];
        if (!var || var->ast_kind != AST_IDENT) {
            sema_report_error(arg, "ref argument must be a local variable");
            return;
        }
        SymEntry* e = symtab_lookup(var->ast_token.text);
        if (!e) {
            sema_report_error(arg, "ref argument must be a local variable");
            return;
        }
        if (e->type.is_const) {
            sema_report_error(arg, "cannot pass const variable '%s' to ref parameter",
                              var->ast_token.text);
        }
        return;
    }
    if (arg->ast_kind == AST_REF_ARG) {
        /* codegen still falls through to the weak-interface emission after
           this diagnostic; that extra cascade is not reproduced here. */
        sema_report_error(arg, "'ref' argument requires a ref parameter");
        return;
    }
    Type* at = &arg->ast_resolved_type;

    if (type_is_null(at)) {
        /* null argument: only reference-like parameters accept it. */
        if (param_type->is_unowned) {
            sema_report_error(arg, "cannot pass null to unowned parameter");
        } else if (!(param_type->is_weak && param_type->type_kind == TYPE_INTERFACE) &&
                   param_type->type_kind != TYPE_INTERFACE &&
                   param_type->type_kind != TYPE_CLASS &&
                   param_type->type_kind != TYPE_OBJECT &&
                   param_type->type_kind != TYPE_VOID) {
            sema_report_error(arg, "cannot pass null to '%s' parameter", type_name(param_type));
        }
        return;
    }

    /* Strict bool/enum rules at the call boundary. */
    if (param_type->type_kind != TYPE_VOID &&
        (bool_mismatch(param_type, at) || enum_mismatch(param_type, at))) {
        sema_report_error(arg, "cannot pass '%s' to '%s' parameter",
                          type_name(at), type_name(param_type));
        return;
    }

    /* A class parameter does not accept object; cast with 'as' first. */
    if (param_type->type_kind == TYPE_CLASS && at->type_kind == TYPE_OBJECT) {
        sema_report_error(arg, "cannot pass 'object' to '%s' parameter; cast with 'as' first",
                          type_name(param_type));
        return;
    }

    /* object parameter: interface (strong), class, and object pass through. */
    if (param_type->type_kind == TYPE_OBJECT) {
        if (!((at->type_kind == TYPE_INTERFACE && !at->is_weak) ||
              at->type_kind == TYPE_CLASS || at->type_kind == TYPE_OBJECT)) {
            sema_report_error(arg, "cannot pass '%s' to 'object' parameter", type_name(at));
        }
        return;
    }

    /* weak interface parameter: weak/strong interface and class accepted. */
    if (param_type->is_weak && param_type->type_kind == TYPE_INTERFACE) {
        if (!((at->is_weak && at->type_kind == TYPE_INTERFACE) ||
              at->type_kind == TYPE_INTERFACE || at->type_kind == TYPE_CLASS)) {
            sema_report_error(arg, "cannot pass this argument to weak interface parameter");
        }
    }
}

/* Runs sema_check_call_arg over the argument list; positions beyond the
   signature (or without one) get a zeroed type, mirroring codegen. */
static void sema_check_call_args(AstNode* node, int param_count, const Type* param_types) {
    AstNode* arg = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
    int idx = 0;
    while (arg) {
        Type expected;
        memset(&expected, 0, sizeof(expected));
        if (param_types && idx < param_count) expected = param_types[idx];
        sema_check_call_arg(arg, &expected);
        idx++;
        arg = arg->next;
    }
}

/* Access control (mirrors codegen's member_visible): a private class member
   is visible only inside methods of the same class (any instance, C++ style).
   s_current_class_name is set by sema_walk_method for class methods and is
   NULL everywhere else (free functions, struct methods, interface default
   methods). */
static const char* s_current_class_name = NULL;
/* Mirrors codegen's ctx->current_method_is_static / is_interface_default_method
   for the 'this' identifier diagnostics. */
static int s_current_method_is_static = 0;
static int s_in_interface_default = 0;

/* Mirrors the AST_IDENT dispatch in codegen_expr: 'this' misuse and unknown
   identifiers.  ('this' outside any method emits no diagnostic in codegen
   either; the parser rejects it.) */
static void sema_check_ident(AstNode* node) {
    if (strcmp(node->ast_token.text, "this") == 0) {
        if (s_current_method_is_static) {
            sema_report_error(node, "'this' cannot be used in a static method");
            return;
        }
        if (s_in_interface_default) {
            sema_report_error(node, "'this' is not allowed in interface default method");
        }
        return;
    }
    if (!symtab_lookup(node->ast_token.text)) {
        sema_report_error(node, "unknown identifier '%s'", node->ast_token.text);
    }
}

/* True for a bare identifier that names a type rather than a variable: enum
   names ('Key.Up') and class names ('Config.MAX', 'ClassName.m()') appear as
   member-access objects, where codegen emits them directly and never runs the
   identifier diagnostics. */
static int sema_ident_is_type_name(AstNode* node) {
    if (!node || node->ast_kind != AST_IDENT) return 0;
    if (symtab_lookup(node->ast_token.text)) return 0;
    return symtab_find_enum(node->ast_token.text) != NULL ||
           symtab_find_class(node->ast_token.text) != NULL;
}

static int sema_member_visible(const char* owner, int is_private) {
    if (!is_private) return 1;
    return s_current_class_name && strcmp(s_current_class_name, owner) == 0;
}

/* Mirrors the diagnostics of codegen_array_method_call (defined below,
   after sema_check_call). */
static void sema_check_array_method_call(AstNode* node, AstNode* callee,
                                         AstNode* arr, const char* mname);
/* Defined below, after sema_check_call. */
static void sema_check_member_access(AstNode* node);
/* Defined below, after sema_check_call. */
static void sema_check_builtin_call(AstNode* node, AstNode* callee);

/* Mirrors the call-boundary diagnostics of codegen_call: callee existence,
   arity, per-argument checks, and method-call visibility, plus the weak/
   unowned lock() and assert/hash/equals builtin checks. */
static void sema_check_call(AstNode* node) {
    AstNode* callee = node->ast_children[0];
    if (!callee) return;
    char dbuf[160];
    int dn;

    if (callee->ast_kind == AST_MEMBER_ACCESS) {
        AstNode* obj = callee->ast_children[0];
        const char* mname = callee->ast_token.text;

        /* Static method call via the class name: ClassName.m(args). */
        ClassInfo* sci = NULL;
        MethodInfo* smi = sema_static_call_method(callee, &sci);
        if (sci) {
            if (!smi) {
                sema_report_error(callee, "method '%s.%s' does not exist", sci->name, mname);
                return;
            }
            if (!smi->method_is_static) {
                sema_report_error(callee, "cannot call instance method '%s.%s' via the class name; use an instance",
                                  sci->name, mname);
                return;  /* codegen returns before the arity check */
            }
            if (!sema_member_visible(sci->name, smi->is_private)) {
                sema_report_error(callee, "cannot call private method '%s.%s'", sci->name, mname);
            }
            dn = snprintf(dbuf, sizeof(dbuf), "%s.%s", sci->name, mname);
            CHECK_SNPRINTF(dn, sizeof(dbuf), "method display name too long");
            sema_check_call_arity(callee, dbuf, sema_count_call_args(node),
                                  smi->param_count, smi->param_defaults);
            sema_check_call_args(node, smi->param_count, smi->param_types);
            return;
        }

        if (!obj) return;
        Type* ot = &obj->ast_resolved_type;
        /* Array builtins have their own fixed-signature checks. */
        if (ot->is_array && is_array_method_name(mname)) {
            sema_check_array_method_call(node, callee, obj, mname);
            return;
        }
        /* lock() on a weak reference is the point of the builtin: no checks.
           unowned references have no lock(); codegen mirrors this error. */
        if (strcmp(mname, "lock") == 0 && ot->is_weak) return;
        if (strcmp(mname, "lock") == 0 && ot->is_unowned) {
            sema_report_error(node, "unowned references do not have lock(); use them directly");
            return;
        }

        if (ot->type_kind == TYPE_CLASS) {
            ClassInfo* ci = ot->type_arg_count > 0
                ? symtab_instantiate_class_from_type(ot)
                : symtab_find_class(ot->class_name);
            MethodInfo* mi = ci ? symtab_find_method_in_class(ci, mname) : NULL;
            if (ci && !mi) {
                sema_report_error(callee, "method '%s.%s' does not exist", ci->name, mname);
            }
            if (mi && mi->method_is_static) {
                sema_report_error(callee, "cannot call static method '%s.%s' via an instance; use the class name",
                                  ci->name, mname);
            }
            if (mi && ci && !sema_member_visible(ci->name, mi->is_private)) {
                sema_report_error(callee, "cannot call private method '%s.%s'", ci->name, mname);
            }
            if (mi && ci) {
                dn = snprintf(dbuf, sizeof(dbuf), "%s.%s", ci->name, mname);
                CHECK_SNPRINTF(dn, sizeof(dbuf), "method display name too long");
                sema_check_call_arity(callee, dbuf, sema_count_call_args(node),
                                      mi->param_count, mi->param_defaults);
            }
            sema_check_call_args(node, mi ? mi->param_count : 0,
                                 mi ? mi->param_types : NULL);
            return;
        }
        if (ot->type_kind == TYPE_STRUCT) {
            StructInfo* si = symtab_find_struct(ot->class_name);
            MethodInfo* mi = si ? symtab_find_struct_method(si, mname) : NULL;
            if (si && !mi) {
                sema_report_error(callee, "method '%s.%s' does not exist", si->name, mname);
            }
            if (mi && si) {
                dn = snprintf(dbuf, sizeof(dbuf), "%s.%s", si->name, mname);
                CHECK_SNPRINTF(dn, sizeof(dbuf), "method display name too long");
                sema_check_call_arity(callee, dbuf, sema_count_call_args(node),
                                      mi->param_count, mi->param_defaults);
            }
            if (obj->ast_kind != AST_IDENT && obj->ast_kind != AST_MEMBER_ACCESS &&
                obj->ast_kind != AST_ARRAY_ACCESS) {
                sema_report_error(obj, "struct method receiver must be a variable, field, or array element");
            }
            sema_check_call_args(node, mi ? mi->param_count : 0,
                                 mi ? mi->param_types : NULL);
            return;
        }
        if (ot->type_kind == TYPE_INTERFACE) {
            InterfaceInfo* ii = symtab_find_interface(ot->class_name);
            InterfaceMethodInfo* im = ii ? symtab_find_interface_method(ii, mname) : NULL;
            if (ii && !im) {
                sema_report_error(callee, "method '%s.%s' does not exist", ii->name, mname);
            }
            if (im && ii) {
                dn = snprintf(dbuf, sizeof(dbuf), "%s.%s", ii->name, mname);
                CHECK_SNPRINTF(dn, sizeof(dbuf), "method display name too long");
                sema_check_call_arity(callee, dbuf, sema_count_call_args(node),
                                      im->param_count, im->param_defaults);
            }
            sema_check_call_args(node, im ? im->param_count : 0,
                                 im ? im->param_types : NULL);
            return;
        }
        /* Other receiver kinds: codegen falls through to emitting the callee
           as an expression, so the member-access read diagnostics and the
           (signature-less) argument checks apply there. */
        sema_check_member_access(callee);
        sema_check_call_args(node, 0, NULL);
        return;
    }

    if (callee->ast_kind == AST_IDENT) {
        FuncInfo* fi = symtab_find_func(callee->ast_token.text);
        if (!fi) {
            /* assert/hash/equals are unregistered builtins; a user-defined
               function of the same name shadows them (fi set above). */
            if (strcmp(callee->ast_token.text, "assert") == 0 ||
                strcmp(callee->ast_token.text, "hash") == 0 ||
                strcmp(callee->ast_token.text, "equals") == 0) {
                sema_check_builtin_call(node, callee);
                return;
            }
            sema_report_error(callee, "unknown function '%s'", callee->ast_token.text);
            return;
        }
        if (fi->is_builtin) {
            /* print: codegen checks only the first argument, never arity. */
            AstNode* arg = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
            if (arg && fi->param_count > 0) {
                sema_check_call_arg(arg, &fi->param_types[0]);
            }
            return;
        }
        sema_check_call_arity(callee, callee->ast_token.text, sema_count_call_args(node),
                              fi->param_count, fi->param_defaults);
        sema_check_call_args(node, fi->param_count, fi->param_types);
    }
    /* other callee shapes: no call-boundary checks (mirrors codegen). */
}

/* Mirrors the diagnostics of the unregistered assert/hash/equals builtins in
   codegen_call, in codegen's branch order (the order sets diagnostic priority
   when several apply).  The callee is a bare identifier with no user-defined
   function of the same name. */
static void sema_check_builtin_call(AstNode* node, AstNode* callee) {
    const char* name = callee->ast_token.text;

    if (strcmp(name, "assert") == 0) {
        AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
        if (!args || args->next) {
            sema_report_error(callee, "assert expects 1 argument");
        }
        return;
    }

    if (strcmp(name, "hash") == 0) {
        AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
        if (!args || args->next) {
            sema_report_error(callee, "hash expects 1 argument");
            return;
        }
        Type* at = &args->ast_resolved_type;
        if (at->is_weak || at->is_unowned) {
            sema_report_error(callee, "cannot hash a weak/unowned reference; lock() it first");
            return;
        }
        if (at->is_array || at->type_kind == TYPE_STRUCT) {
            sema_report_error(callee, "cannot hash values of this type");
            return;
        }
        /* A class implementing IHashable hashes through its own hash(). */
        if (at->type_kind == TYPE_CLASS) {
            ClassInfo* ci = class_info_for_type(at);
            if (class_implements(ci, "IHashable")) {
                MethodInfo* mi = symtab_find_method_in_class(ci, "hash");
                if (mi && mi->is_private) {
                    sema_report_error(callee, "hash() method of '%s' is private", ci->name);
                }
            }
        }
        return;
    }

    /* equals(a, b) */
    AstNode* a = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
    AstNode* b = a ? a->next : NULL;
    if (!a || !b || b->next) {
        sema_report_error(callee, "equals expects 2 arguments");
        return;
    }
    Type* at = &a->ast_resolved_type;
    Type* bt = &b->ast_resolved_type;
    if (at->is_weak || at->is_unowned || bt->is_weak || bt->is_unowned) {
        sema_report_error(callee, "cannot compare weak/unowned references with equals; lock() them first");
        return;
    }
    if (at->is_array || bt->is_array || at->type_kind == TYPE_STRUCT || bt->type_kind == TYPE_STRUCT) {
        sema_report_error(callee, "cannot compare values of this type with equals");
        return;
    }
    int a_ref = type_is_reference(at) || type_is_null(at);
    int b_ref = type_is_reference(bt) || type_is_null(bt);
    if (a_ref != b_ref) {
        sema_report_error(callee, "cannot compare '%s' with '%s'", type_name(at), type_name(bt));
        return;
    }
    if (!a_ref) return;  /* bool, integer, and float values compare directly */
    if (type_is_null(at) && type_is_null(bt)) return;
    if (at->type_kind == TYPE_CLASS && strcmp(at->class_name, "String") == 0) {
        if (!type_is_null(bt) &&
            !(bt->type_kind == TYPE_CLASS && strcmp(bt->class_name, "String") == 0)) {
            sema_report_error(callee, "cannot compare 'string' with '%s'", type_name(bt));
        }
        return;
    }
    if (at->type_kind == TYPE_INTERFACE) return;
    if (at->type_kind == TYPE_CLASS && !type_is_null(at)) {
        ClassInfo* ci = class_info_for_type(at);
        if (class_implements(ci, "IHashable")) {
            MethodInfo* mi = symtab_find_method_in_class(ci, "equals");
            if (mi && mi->is_private) {
                sema_report_error(callee, "equals(object) method of '%s' is private", ci->name);
            }
        }
    }
}

/* Mirrors the diagnostics of codegen_array_method_call (array builtins).
   The "unknown array method" fallback is unreachable there (the caller gates
   on is_array_method_name), so it has no sema counterpart. */
static void sema_check_array_method_call(AstNode* node, AstNode* callee,
                                         AstNode* arr, const char* mname) {
    AstNode* args = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;

    if (strcmp(mname, "length") == 0 || strcmp(mname, "capacity") == 0) {
        /* Fixed zero-parameter signatures (checked at the call dispatch). */
        sema_check_call_arity(callee, mname, sema_count_call_args(node), 0, NULL);
        return;
    }
    if (strcmp(mname, "push") == 0) {
        if (!args) {
            sema_report_error(arr, "push() requires a value argument");
            return;
        }
        /* object arrays take interface/class/object/null values only. */
        Type et = array_elem_type(&arr->ast_resolved_type);
        if (et.type_kind == TYPE_OBJECT) {
            Type* at = &args->ast_resolved_type;
            if (!(at->type_kind == TYPE_INTERFACE && !at->is_weak) &&
                at->type_kind != TYPE_CLASS && at->type_kind != TYPE_OBJECT &&
                !type_is_null(at)) {
                sema_report_error(args, "cannot push '%s' to an 'object' array", type_name(at));
            }
        }
        return;
    }
    if (strcmp(mname, "reserve") == 0) {
        if (!args) sema_report_error(arr, "reserve() requires a capacity argument");
        return;
    }
    if (strcmp(mname, "resize") == 0) {
        if (!args) sema_report_error(arr, "resize() requires a length argument");
        return;
    }
    if (strcmp(mname, "move_to") == 0 || strcmp(mname, "copy_to") == 0) {
        if (!args || args->ast_kind != AST_REF_ARG) {
            sema_report_error(arr, "%s() requires a ref destination argument", mname);
            return;
        }
        /* Mirrors emit_array_ref_arg: the destination must be a local. */
        AstNode* dest = args->ast_children[0];
        if (!dest || dest->ast_kind != AST_IDENT ||
            !symtab_lookup(dest->ast_token.text)) {
            sema_report_error(dest, "array move/copy destination must be a local variable");
        }
        return;
    }
    /* pop/clear/compact: no diagnostics. */
}

/* Mirrors the diagnostic of codegen_array_access. */
static void sema_check_array_access(AstNode* node) {
    AstNode* arr = node->ast_children[0];
    if (arr && type_is_null(&arr->ast_resolved_type)) {
        sema_report_error(node, "cannot index null");
    }
}

/* -------------------------------------------------------------------------
   Member access checks (mirrors codegen_member_access)
   ------------------------------------------------------------------------- */

/* Mirrors the diagnostics of codegen_member_access.  Runs on read positions
   (see sema_walk_target for write positions and sema_check_call for call
   callees). */
static void sema_check_member_access(AstNode* node) {
    AstNode* obj = node->ast_children[0];
    if (!obj) return;

    /* Enum variant access 'Key.Up'.  A local variable of the same name
       shadows the enum (same rule as class static calls). */
    if (obj->ast_kind == AST_IDENT && !symtab_lookup(obj->ast_token.text)) {
        EnumInfo* ei = symtab_find_enum(obj->ast_token.text);
        if (ei) {
            int found = 0;
            int i;
            for (i = 0; i < ei->variant_count; i++) {
                if (strcmp(ei->variant_names[i], node->ast_token.text) == 0) { found = 1; break; }
            }
            if (!found) {
                sema_report_error(node, "enum '%s' has no variant '%s'", ei->name, node->ast_token.text);
            }
            return;
        }
    }
    /* Static class const access 'Config.MAX' (same shadowing rule). */
    if (obj->ast_kind == AST_IDENT && !symtab_lookup(obj->ast_token.text)) {
        ClassInfo* ci = symtab_find_class(obj->ast_token.text);
        if (ci && !ci->is_generic) {
            ConstInfo* cc = symtab_find_class_const(ci->name, node->ast_token.text);
            if (cc) {
                if (!sema_member_visible(ci->name, cc->is_private)) {
                    sema_report_error(node, "static const '%s.%s' is private", ci->name, cc->name);
                }
                return;
            }
        }
    }

    Type* ot = &obj->ast_resolved_type;
    if (type_is_null(ot)) {
        sema_report_error(node, "cannot access member '%s' on null", node->ast_token.text);
        return;
    }
    if (ot->type_kind == TYPE_OBJECT && !ot->is_array) {
        sema_report_error(node, "cannot access member '%s' on object; cast it with 'as' first",
                          node->ast_token.text);
        return;
    }
    if (ot->is_array) {
        /* Arrays have no member fields; all operations are builtin methods
           dispatched as calls. */
        sema_report_error(node, "array has no member '%s'", node->ast_token.text);
        return;
    }
    if (ot->type_kind == TYPE_CLASS) {
        ClassInfo* ci = ot->type_arg_count > 0
            ? symtab_instantiate_class_from_type(ot)
            : symtab_find_class(ot->class_name);
        if (ci) {
            int found = 0;
            int i;
            for (i = 0; i < ci->field_count; i++) {
                if (strcmp(ci->field_names[i], node->ast_token.text) == 0) {
                    found = 1;
                    if (!sema_member_visible(ci->name, ci->field_private[i])) {
                        sema_report_error(node, "cannot access private field '%s.%s'",
                                          ci->name, node->ast_token.text);
                    }
                    break;
                }
            }
            if (!found) {
                PropertyInfo* pi = symtab_find_property(ci, node->ast_token.text);
                if (pi) {
                    /* A valid read lowers to the getter call in codegen;
                       only the invalid cases reach the diagnostics. */
                    if (!pi->has_get) {
                        sema_report_error(node, "property '%s.%s' has no getter", ci->name, pi->name);
                    } else if (!sema_member_visible(ci->name, pi->is_private)) {
                        sema_report_error(node, "cannot access private property '%s.%s'", ci->name, pi->name);
                    }
                    return;
                }
                sema_report_error(node, "class '%s' has no field '%s'", ci->name, node->ast_token.text);
            }
        }
        return;
    }
    if (ot->type_kind == TYPE_STRUCT) {
        StructInfo* si = symtab_find_struct(ot->class_name);
        if (si) {
            int found = 0;
            int i;
            for (i = 0; i < si->field_count; i++) {
                if (strcmp(si->field_names[i], node->ast_token.text) == 0) { found = 1; break; }
            }
            if (!found) {
                sema_report_error(node, "struct '%s' has no field '%s'", si->name, node->ast_token.text);
            }
        }
    }
}

/* Mirrors the diagnostics of the AST_INC_DEC dispatch in codegen_expr.
   (The "not allowed in ..." placement errors are parser-stage.) */
static void sema_check_inc_dec(AstNode* node) {
    AstNode* operand = node->ast_children[0];
    if (!operand) return;
    /* Property ++/-- is left unlowered by prepare precisely so this
       diagnostic fires; sema sees the same raw shape. */
    if (member_access_property(operand, NULL)) {
        sema_report_error(node, "increment/decrement is not supported on properties");
        return;
    }
    Type* t = &operand->ast_resolved_type;
    if (!type_is_numeric(t)) {
        sema_report_error(node, "increment/decrement not supported for this type");
        return;
    }
    if (t->is_const) {
        sema_report_error(node, "cannot modify const variable '%s'", operand->ast_token.text);
    }
}

/* -------------------------------------------------------------------------
   Match checks (mirrors codegen_match_stmt)
   ------------------------------------------------------------------------- */

/* Mirrors the per-arm diagnostics of codegen_match_stmt.  The pattern type
   is read from arm->ast_resolved_type (set at parse time). */
static void sema_check_match_arm(AstNode* arm, const Type* expr_type) {
    Type pat_type = arm->ast_resolved_type;

    if (pat_type.type_kind == TYPE_VOID) return;  /* else arm */

    if (pat_type.type_kind == TYPE_I32) {
        if (!type_is_integer(expr_type)) {
            sema_report_error(arm, "integer match pattern cannot match expression of type '%s'",
                              type_name(expr_type));
        }
        return;
    }
    if (pat_type.type_kind == TYPE_ENUM) {
        /* Enum variant constant arm.  No exhaustiveness check. */
        if (expr_type->type_kind != TYPE_ENUM ||
            strcmp(expr_type->class_name, pat_type.class_name) != 0) {
            sema_report_error(arm, "match pattern type '%s' does not match expression type '%s'",
                              type_name(&pat_type), type_name(expr_type));
        }
        EnumInfo* ei = symtab_find_enum(pat_type.class_name);
        int found = 0;
        int vi;
        for (vi = 0; ei && vi < ei->variant_count; vi++) {
            if (strcmp(ei->variant_names[vi], arm->ast_token.text) == 0) { found = 1; break; }
        }
        if (!found) {
            sema_report_error(arm, "enum '%s' has no variant '%s'",
                              pat_type.class_name, arm->ast_token.text);
        }
        return;
    }
    if (pat_type.type_kind == TYPE_CLASS) {
        if (expr_type->type_kind == TYPE_INTERFACE) {
            ClassInfo* cls = symtab_find_class(pat_type.class_name);
            InterfaceInfo* iface = symtab_find_interface(expr_type->class_name);
            if (!(cls && iface && class_implements(cls, expr_type->class_name))) {
                sema_report_error(arm, "class '%s' does not implement interface '%s'",
                                  pat_type.class_name, expr_type->class_name);
            }
        } else if (expr_type->type_kind == TYPE_CLASS ||
                   expr_type->type_kind == TYPE_OBJECT) {
            /* object matches any class pattern; a plain class expression
               keeps the exact-name requirement. */
            if (expr_type->type_kind == TYPE_CLASS &&
                strcmp(expr_type->class_name, pat_type.class_name) != 0) {
                sema_report_error(arm, "match pattern type '%s' does not match expression type '%s'",
                                  pat_type.class_name, expr_type->class_name);
            }
        } else {
            sema_report_error(arm, "class match pattern cannot match expression of type '%s'",
                              type_name(expr_type));
        }
        return;
    }
    sema_report_error(arm, "unsupported match pattern type");
}

/* Mirrors the match-expression diagnostics of codegen_match_stmt. */
static void sema_check_match(AstNode* node) {
    AstNode* expr = (node->ast_child_count > 0) ? node->ast_children[0] : NULL;
    if (!expr) return;
    Type* expr_type = &expr->ast_resolved_type;
    if (expr_type->is_unowned) {
        sema_report_error(expr, "cannot match on an unowned reference; convert it to a strong reference first");
    }
    if (type_is_null(expr_type)) {
        sema_report_error(expr, "cannot match on null");
    }
}

/* Walks an assignment/inc-dec target.  A property member access in a write
   position is NOT read-checked: its dedicated diagnostics come from the
   assign/inc-dec dispatches (or the access is lowered away in codegen), so
   only the object expression is walked as a read.  All other targets behave
   as ordinary read positions (mirroring the emission-time checks codegen
   runs on them). */
static void sema_walk_expr(AstNode* node);

/* Walks the object of a member access.  A bare identifier is a read position
   unless it names a type (enum/class), which codegen emits directly without
   the identifier diagnostics. */
static void sema_walk_member_obj(AstNode* obj) {
    if (!obj) return;
    if (obj->ast_kind == AST_IDENT) {
        sema_resolve_type(obj);
        if (!sema_ident_is_type_name(obj)) {
            sema_check_ident(obj);
        }
        sema_walk_expr(obj->next);
        return;
    }
    sema_walk_expr(obj);
}

static void sema_walk_target(AstNode* node) {
    if (!node) return;
    if (node->ast_kind == AST_MEMBER_ACCESS && member_access_property(node, NULL)) {
        sema_walk_member_obj(node->ast_children[0]);
        sema_walk_expr(node->next);
        sema_resolve_type(node);
        return;
    }
    sema_walk_expr(node);
}

/* Resolves every node of an expression tree (post-order) and runs the
   migrated checks.  Call argument lists are linked through 'next', so those
   are walked too. */
static void sema_walk_expr(AstNode* node) {
    if (!node) return;
    int i;
    if (node->ast_kind == AST_ASSIGN) {
        /* The LHS is a write position (see sema_walk_target). */
        sema_walk_target(node->ast_children[0]);
        for (i = 1; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
            sema_walk_expr(node->ast_children[i]);
        }
        sema_walk_expr(node->next);
        sema_resolve_type(node);
        sema_check_assign(node);
        return;
    }
    if (node->ast_kind == AST_INC_DEC) {
        sema_walk_target(node->ast_children[0]);
        sema_walk_expr(node->next);
        sema_resolve_type(node);
        sema_check_inc_dec(node);
        return;
    }
    if (node->ast_kind == AST_CALL && node->ast_child_count > 0 &&
        node->ast_children[0] && node->ast_children[0]->ast_kind == AST_MEMBER_ACCESS) {
        /* The callee member access is dispatched by sema_check_call (method
           paths never read-check it); its object is a read position. */
        AstNode* callee = node->ast_children[0];
        sema_walk_member_obj(callee->ast_children[0]);
        for (i = 1; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
            sema_walk_expr(node->ast_children[i]);
        }
        sema_walk_expr(node->next);
        sema_resolve_type(callee);
        sema_resolve_type(node);
        sema_check_call(node);
        return;
    }
    if (node->ast_kind == AST_CALL && node->ast_child_count > 0 &&
        node->ast_children[0] && node->ast_children[0]->ast_kind == AST_IDENT) {
        /* The callee is a function name: sema_check_call resolves it via the
           func table (and the builtin names); it is not a variable read. */
        AstNode* callee = node->ast_children[0];
        for (i = 1; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
            sema_walk_expr(node->ast_children[i]);
        }
        sema_walk_expr(node->next);
        sema_resolve_type(callee);
        sema_resolve_type(node);
        sema_check_call(node);
        return;
    }
    if (node->ast_kind == AST_MEMBER_ACCESS) {
        /* The object is a read position, except for bare type names. */
        sema_walk_member_obj(node->ast_child_count > 0 ? node->ast_children[0] : NULL);
        for (i = 1; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
            sema_walk_expr(node->ast_children[i]);
        }
        sema_walk_expr(node->next);
        sema_resolve_type(node);
        sema_check_member_access(node);
        return;
    }
    for (i = 0; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
        sema_walk_expr(node->ast_children[i]);
    }
    sema_walk_expr(node->next);
    sema_resolve_type(node);
    if (node->ast_kind == AST_BINARY) {
        sema_check_binary(node);
    } else if (node->ast_kind == AST_UNARY) {
        sema_check_unary(node);
    } else if (node->ast_kind == AST_CALL) {
        sema_check_call(node);
    } else if (node->ast_kind == AST_ARRAY_ACCESS) {
        sema_check_array_access(node);
    } else if (node->ast_kind == AST_IDENT) {
        sema_check_ident(node);
    }
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
            sema_check_var_decl(node);
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
            AstNode* expr = (node->ast_child_count > 0) ? node->ast_children[0] : NULL;
            if (expr) sema_walk_expr(expr);
            sema_check_match(node);
            Type expr_type;
            memset(&expr_type, 0, sizeof(expr_type));
            if (expr) expr_type = expr->ast_resolved_type;
            AstNode* arm = (node->ast_child_count > 1) ? node->ast_children[1] : NULL;
            while (arm) {
                sema_check_match_arm(arm, &expr_type);
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
            sema_check_return(node);
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

/* Declaration-level signature checks, migrated from codegen_func_decl,
   codegen_method_decl, and codegen_struct_method_decl.  Only the ref-struct-
   array parameter check lives here: the array-return-by-value and array-
   parameter-must-be-ref diagnostics are already reported by the parser
   (parser.c, with its own texts), and parser errors abort before sema runs,
   so sema copies would be dead code for non-generic declarations; the codegen
   copies remain as the backstop for generic instantiation bodies.
   check_params is 0 for native class methods (codegen returns before the
   parameter loop there), 1 otherwise. */
static void sema_check_signature(AstNode* node, int check_params) {
    if (!check_params) return;
    AstNode* p = (node->ast_child_count == 2) ? node->ast_children[0] : NULL;
    while (p) {
        if (type_is_ref_struct_array(&p->ast_resolved_type)) {
            sema_report_error(p, "arrays of struct '%s' with reference fields are not supported yet",
                              p->ast_resolved_type.class_name);
        }
        p = p->next;
    }
}

/* Walks a function/method body with the given parameters in scope, inserting
   'this' when insert_this is set (mirrors the three codegen emit sites).
   ret_type becomes the current return type for sema_check_return. */
static void sema_walk_body(AstNode* params, AstNode* body,
                           int insert_this, Type* thiz_type, Type* ret_type) {
    Type prev_ret_type = s_current_ret_type;
    s_current_ret_type = *ret_type;
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
    s_current_ret_type = prev_ret_type;
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
    const char* prev_class_name = s_current_class_name;
    s_current_class_name = is_struct ? NULL : owner_name;
    int prev_static = s_current_method_is_static;
    s_current_method_is_static = m->ast_is_static;
    /* codegen returns before the parameter loop for native class methods. */
    sema_check_signature(m, is_struct || !m->ast_is_native);
    sema_walk_body(params, body, insert_this, &thiz_type, &m->ast_resolved_type);
    s_current_method_is_static = prev_static;
    s_current_class_name = prev_class_name;
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
                /* Fields of type (ref-struct)[] are rejected (mirrors
                   codegen_class_decl). */
                int i;
                for (i = 0; i < ci->field_count; i++) {
                    if (type_is_ref_struct_array(&ci->field_types[i])) {
                        sema_report_error(decl, "arrays of struct '%s' with reference fields are not supported yet (field '%s')",
                                          ci->field_types[i].class_name, ci->field_names[i]);
                    }
                }
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
            sema_check_signature(decl, 1);
            sema_walk_body(params, body, 0, NULL, &decl->ast_resolved_type);
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
                Type prev_ret_type = s_current_ret_type;
                s_current_ret_type = im->return_type;
                int prev_iface_default = s_in_interface_default;
                s_in_interface_default = 1;
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
                s_in_interface_default = prev_iface_default;
                s_current_ret_type = prev_ret_type;
            }
            ii = ii->next;
        }
    }
}
