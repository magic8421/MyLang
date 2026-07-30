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

/* Resolves every node of an expression tree (post-order) and runs the
   migrated checks.  Call argument lists are linked through 'next', so those
   are walked too. */
static void sema_walk_expr(AstNode* node) {
    if (!node) return;
    int i;
    for (i = 0; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
        sema_walk_expr(node->ast_children[i]);
    }
    sema_walk_expr(node->next);
    sema_resolve_type(node);
    if (node->ast_kind == AST_ASSIGN) {
        sema_check_assign(node);
    } else if (node->ast_kind == AST_BINARY) {
        sema_check_binary(node);
    } else if (node->ast_kind == AST_UNARY) {
        sema_check_unary(node);
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
    sema_walk_body(params, body, insert_this, &thiz_type, &m->ast_resolved_type);
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
                s_current_ret_type = prev_ret_type;
            }
            ii = ii->next;
        }
    }
}
