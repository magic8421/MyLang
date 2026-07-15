#include "symtab.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static Scope*         current_scope = NULL;
static int            scope_counter = 0;
ClassInfo*            class_list    = NULL;
static StructInfo*    struct_list   = NULL;
InterfaceInfo*        interface_list = NULL;
static FuncInfo*      func_list     = NULL;
static int            next_type_id  = TYPE_ID_CLASS_BASE;

static unsigned hash_string(const char* s) {
    unsigned h = 5381;
    while (*s) {
        h = ((h << 5) + h) + (unsigned char)*s++;
    }
    return h % HASH_SIZE;
}

void symtab_init(void) {
    class_list  = NULL;
    scope_counter = 0;
    current_scope = calloc(1, sizeof(Scope));
    current_scope->level = scope_counter++;
    next_type_id = TYPE_ID_CLASS_BASE;
}

void symtab_enter_scope(void) {
    Scope* s = calloc(1, sizeof(Scope));
    s->parent = current_scope;
    s->level  = scope_counter++;
    current_scope = s;
}

void symtab_exit_scope(void) {
    if (current_scope && current_scope->parent) {
        Scope* old = current_scope;
        current_scope = current_scope->parent;
        free(old);
    }
}

Scope* symtab_current_scope(void) {
    return current_scope;
}

void symtab_insert(const char* name, Type type) {
    unsigned idx = hash_string(name);
    SymEntry* e  = malloc(sizeof(SymEntry));
    CHECK_STRSCPY(strscpy(e->name, name, sizeof(e->name)), "symbol name too long");
    e->type = type;
    e->next = current_scope->table[idx];
    current_scope->table[idx] = e;
}

SymEntry* symtab_lookup(const char* name) {
    Scope* s = current_scope;
    while (s) {
        unsigned idx = hash_string(name);
        SymEntry* e = s->table[idx];
        while (e) {
            if (strcmp(e->name, name) == 0) {
                return e;
            }
            e = e->next;
        }
        s = s->parent;
    }
    return NULL;
}

SymEntry* symtab_lookup_current(const char* name) {
    unsigned idx = hash_string(name);
    SymEntry* e = current_scope->table[idx];
    while (e) {
        if (strcmp(e->name, name) == 0) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

void symtab_add_class(const char* name, ClassInfo* info) {
    info->next = class_list;
    class_list = info;
}

ClassInfo* symtab_find_class(const char* name) {
    ClassInfo* s = class_list;
    while (s) {
        if (strcmp(s->name, name) == 0) {
            return s;
        }
        s = s->next;
    }
    return NULL;
}

int symtab_add_field(ClassInfo* info, const char* name, Type type) {
    if (info->field_count >= MAX_FIELDS) return -1;
    CHECK_STRSCPY(strscpy(info->field_names[info->field_count], name, sizeof(info->field_names[0])), "field name too long");
    info->field_types[info->field_count] = type;
    info->field_count++;
    return 0;
}

void symtab_add_struct(const char* name, StructInfo* info) {
    info->next = struct_list;
    struct_list = info;
}

StructInfo* symtab_find_struct(const char* name) {
    StructInfo* s = struct_list;
    while (s) {
        if (strcmp(s->name, name) == 0) return s;
        s = s->next;
    }
    return NULL;
}

int symtab_add_struct_field(StructInfo* info, const char* name, Type type) {
    if (info->field_count >= MAX_FIELDS) return -1;
    CHECK_STRSCPY(strscpy(info->field_names[info->field_count], name, sizeof(info->field_names[0])), "field name too long");
    info->field_types[info->field_count] = type;
    info->field_count++;
    return 0;
}

void symtab_add_func(const char* name, Type ret_type,
                     int pc, const char pn[][64], const Type pt[]) {
    FuncInfo* f = calloc(1, sizeof(FuncInfo));
    CHECK_STRSCPY(strscpy(f->name, name, sizeof(f->name)), "function name too long");
    f->return_type = ret_type;
    f->param_count = pc;
    int i;
    for (i = 0; i < pc && i < 16; i++) {
        CHECK_STRSCPY(strscpy(f->param_names[i], pn[i], sizeof(f->param_names[i])), "parameter name too long");
        f->param_types[i] = pt[i];
    }
    f->next = func_list;
    func_list = f;
}

FuncInfo* symtab_find_func(const char* name) {
    FuncInfo* f = func_list;
    while (f) {
        if (strcmp(f->name, name) == 0) return f;
        f = f->next;
    }
    return NULL;
}

void symtab_add_method(ClassInfo* cls, const char* name, Type ret_type,
                       int pc, const char pn[][64], const Type pt[]) {
    MethodInfo* m = calloc(1, sizeof(MethodInfo));
    CHECK_STRSCPY(strscpy(m->name, name, sizeof(m->name)), "method name too long");
    m->return_type = ret_type;
    m->param_count = pc;
    int i;
    for (i = 0; i < pc && i < 16; i++) {
        CHECK_STRSCPY(strscpy(m->param_names[i], pn[i], sizeof(m->param_names[i])), "parameter name too long");
        m->param_types[i] = pt[i];
    }
    m->next = cls->methods;
    cls->methods = m;
}

MethodInfo* symtab_find_method(const char* class_name, const char* method_name) {
    ClassInfo* cls = symtab_find_class(class_name);
    if (!cls) return NULL;
    MethodInfo* m = cls->methods;
    while (m) {
        if (strcmp(m->name, method_name) == 0) return m;
        m = m->next;
    }
    return NULL;
}

MethodInfo* symtab_find_method_in_class(ClassInfo* cls, const char* method_name) {
    if (!cls) return NULL;
    MethodInfo* m = cls->methods;
    while (m) {
        if (strcmp(m->name, method_name) == 0) return m;
        m = m->next;
    }
    return NULL;
}

void symtab_add_interface(const char* name, InterfaceInfo* info) {
    info->next = interface_list;
    interface_list = info;
}

InterfaceInfo* symtab_find_interface(const char* name) {
    InterfaceInfo* s = interface_list;
    while (s) {
        if (strcmp(s->name, name) == 0) return s;
        s = s->next;
    }
    return NULL;
}

void symtab_add_interface_method(InterfaceInfo* iface, const char* name,
                                  Type ret_type, int pc,
                                  const char pn[][64], const Type pt[]) {
    if (iface->method_count >= MAX_IFACE_METHODS) return;
    InterfaceMethodInfo* m = &iface->methods[iface->method_count++];
    CHECK_STRSCPY(strscpy(m->name, name, sizeof(m->name)), "interface method name too long");
    m->return_type = ret_type;
    m->param_count = pc;
    int i;
    for (i = 0; i < pc && i < 16; i++) {
        CHECK_STRSCPY(strscpy(m->param_names[i], pn[i], sizeof(m->param_names[i])), "parameter name too long");
        m->param_types[i] = pt[i];
    }
}

InterfaceMethodInfo* symtab_find_interface_method(InterfaceInfo* iface,
                                                   const char* method_name) {
    int i;
    for (i = 0; i < iface->method_count; i++) {
        if (strcmp(iface->methods[i].name, method_name) == 0) {
            return &iface->methods[i];
        }
    }
    return NULL;
}

void symtab_add_class_impl(ClassInfo* cls, const char* iface_name) {
    if (cls->impl_count >= MAX_IMPL) {
        fprintf(stderr, "error: class '%s' implements too many interfaces (max %d)\n",
                cls->name, MAX_IMPL);
        return;
    }
    CHECK_STRSCPY(strscpy(cls->impl_names[cls->impl_count], iface_name,
                          sizeof(cls->impl_names[0])), "interface name too long");
    cls->impl_count++;
}

static int signature_matches(MethodInfo* cls_method, InterfaceMethodInfo* iface_method) {
    if (!type_equal(&cls_method->return_type, &iface_method->return_type)) {
        return 0;
    }
    if (cls_method->param_count != iface_method->param_count) {
        return 0;
    }
    int i;
    for (i = 0; i < cls_method->param_count; i++) {
        if (!type_equal(&cls_method->param_types[i], &iface_method->param_types[i])) {
            return 0;
        }
    }
    return 1;
}

static int iface_method_signature_matches(InterfaceMethodInfo* a, InterfaceMethodInfo* b) {
    if (!type_equal(&a->return_type, &b->return_type)) {
        return 0;
    }
    if (a->param_count != b->param_count) {
        return 0;
    }
    int i;
    for (i = 0; i < a->param_count; i++) {
        if (!type_equal(&a->param_types[i], &b->param_types[i])) {
            return 0;
        }
    }
    return 1;
}

int symtab_validate_impls(void) {
    int errors = 0;
    ClassInfo* ci = class_list;
    while (ci) {
        int i;
        for (i = 0; i < ci->impl_count && i < MAX_IMPL; i++) {
            const char* iname = ci->impl_names[i];
            InterfaceInfo* ii = symtab_find_interface(iname);
            if (!ii) {
                fprintf(stderr, "error: class '%s' implements unknown interface '%s'\n",
                        ci->name, iname);
                errors++;
                continue;
            }
            ci->impl_infos[i] = ii;

            /* check duplicate implementations */
            int j;
            for (j = 0; j < i; j++) {
                if (ci->impl_infos[j] == ii) {
                    fprintf(stderr, "error: class '%s' implements interface '%s' more than once\n",
                            ci->name, iname);
                    errors++;
                    ci->impl_infos[i] = NULL;
                    break;
                }
            }
            if (!ci->impl_infos[i]) continue;

            /* check for method signature conflicts between implemented interfaces */
            for (j = 0; j < i; j++) {
                InterfaceInfo* other = ci->impl_infos[j];
                if (!other) continue;
                int m;
                for (m = 0; m < ii->method_count; m++) {
                    InterfaceMethodInfo* im = &ii->methods[m];
                    int n;
                    for (n = 0; n < other->method_count; n++) {
                        InterfaceMethodInfo* om = &other->methods[n];
                        if (strcmp(im->name, om->name) != 0) continue;
                        if (!iface_method_signature_matches(im, om)) {
                            fprintf(stderr, "error: class '%s' cannot implement both '%s.%s()' and '%s.%s()' with conflicting signatures\n",
                                    ci->name, ii->name, im->name, other->name, om->name);
                            errors++;
                        }
                    }
                }
            }

            /* check every interface method is implemented */
            int m;
            for (m = 0; m < ii->method_count; m++) {
                InterfaceMethodInfo* im = &ii->methods[m];
                MethodInfo* cls_m = ci->methods;
                int found = 0;
                while (cls_m) {
                    if (strcmp(cls_m->name, im->name) == 0) {
                        if (signature_matches(cls_m, im)) {
                            found = 1;
                            break;
                        } else {
                            fprintf(stderr, "error: method '%s' in class '%s' has wrong signature for interface '%s'\n",
                                    im->name, ci->name, iname);
                            errors++;
                            found = -1;
                            break;
                        }
                    }
                    cls_m = cls_m->next;
                }
                if (found == 0) {
                    fprintf(stderr, "error: class '%s' does not implement '%s.%s()'\n",
                            ci->name, iname, im->name);
                    errors++;
                }
            }
        }
        ci = ci->next;
    }
    return errors;
}

int symtab_next_type_id(void) {
    return next_type_id++;
}

void symtab_mark_class_generic(ClassInfo* info, AstNode* ast, int param_count, const char* params[]) {
    int i;
    info->is_generic = 1;
    info->generic_ast = ast;
    info->generic_param_count = param_count;
    for (i = 0; i < param_count && i < MAX_GENERIC_PARAMS; i++) {
        CHECK_STRSCPY(strscpy(info->generic_params[i], params[i], sizeof(info->generic_params[i])),
                      "generic parameter name too long");
    }
}

ClassInfo* symtab_find_class_by_mangled(const char* mangled_name) {
    ClassInfo* s = class_list;
    while (s) {
        if (s->is_instantiation && strcmp(s->mangled_name, mangled_name) == 0) {
            return s;
        }
        s = s->next;
    }
    return NULL;
}

ClassInfo* symtab_add_class_instantiation(ClassInfo* generic_def, const Type** args, int arg_count) {
    if (arg_count != generic_def->generic_param_count) {
        fprintf(stderr, "error: generic class '%s' expects %d type arguments, got %d\n",
                generic_def->name, generic_def->generic_param_count, arg_count);
        return NULL;
    }

    Type inst_type = type_make_user(TYPE_CLASS, generic_def->name);
    int i;
    for (i = 0; i < arg_count && i < MAX_TYPE_ARGS; i++) {
        type_set_arg(&inst_type, i, args[i]);
    }
    const char* mangled = type_mangled_name(&inst_type);

    ClassInfo* existing = symtab_find_class_by_mangled(mangled);
    if (existing) {
        for (i = 0; i < inst_type.type_arg_count && i < MAX_TYPE_ARGS; i++) {
            type_free(inst_type.type_args[i]);
        }
        return existing;
    }

    ClassInfo* ci = calloc(1, sizeof(ClassInfo));
    CHECK_STRSCPY(strscpy(ci->name, mangled, sizeof(ci->name)), "class name too long");
    CHECK_STRSCPY(strscpy(ci->mangled_name, mangled, sizeof(ci->mangled_name)), "mangled class name too long");
    ci->type_id = symtab_next_type_id();
    ci->is_instantiation = 1;
    ci->generic_def = generic_def;
    ci->instantiation_arg_count = arg_count;
    for (i = 0; i < arg_count && i < MAX_GENERIC_PARAMS; i++) {
        ci->instantiation_args[i] = *type_new(args[i]);
    }

    for (i = 0; i < inst_type.type_arg_count && i < MAX_TYPE_ARGS; i++) {
        type_free(inst_type.type_args[i]);
    }

    ci->next = class_list;
    class_list = ci;
    return ci;
}

static int type_implements_interface(const Type* t, const char* iface_name) {
    if (t->type_kind == TYPE_CLASS) {
        ClassInfo* ci = symtab_find_class(t->class_name);
        if (ci) {
            int i;
            for (i = 0; i < ci->impl_count && i < MAX_IMPL; i++) {
                if (strcmp(ci->impl_names[i], iface_name) == 0) return 1;
            }
        }
    }
    if (t->type_kind == TYPE_INTERFACE && strcmp(t->class_name, iface_name) == 0) {
        return 1;
    }
    return 0;
}

/* Validate that method calls on type parameters inside a generic class are only
   allowed when the type parameter has a matching interface constraint. */

typedef struct {
    char names[32][64];
    Type types[32];
    int count;
} LocalScope;

static void local_scope_push(LocalScope* scope, const char* name, Type type) {
    if (scope->count >= 32) return;
    CHECK_STRSCPY(strscpy(scope->names[scope->count], name, sizeof(scope->names[0])),
                  "local scope name too long");
    scope->types[scope->count] = type;
    scope->count++;
}

static Type* local_scope_find(LocalScope* scope, const char* name) {
    int i;
    for (i = scope->count - 1; i >= 0; i--) {
        if (strcmp(scope->names[i], name) == 0) {
            return &scope->types[i];
        }
    }
    return NULL;
}

static int class_find_field_type(ClassInfo* cls, const char* name, Type* out) {
    int i;
    for (i = 0; i < cls->field_count; i++) {
        if (strcmp(cls->field_names[i], name) == 0) {
            *out = cls->field_types[i];
            return 1;
        }
    }
    return 0;
}

static int type_param_has_interface_method(ClassInfo* cls, const char* param_name, const char* method_name) {
    int p;
    for (p = 0; p < cls->generic_param_count && p < MAX_GENERIC_PARAMS; p++) {
        if (strcmp(cls->generic_params[p], param_name) != 0) continue;
        int c;
        for (c = 0; c < cls->generic_constraint_count[p] && c < MAX_CONSTRAINTS_PER_PARAM; c++) {
            const char* iname = cls->generic_constraints[p][c];
            InterfaceInfo* ii = symtab_find_interface(iname);
            if (ii && symtab_find_interface_method(ii, method_name)) {
                return 1;
            }
        }
    }
    return 0;
}

static Type resolve_expr_type(AstNode* node, ClassInfo* cls, LocalScope* scope);

static Type resolve_member_type(AstNode* node, ClassInfo* cls, LocalScope* scope) {
    Type void_t = {0};
    void_t.type_kind = TYPE_VOID;
    if (!node) return void_t;
    if (node->ast_kind != AST_MEMBER_ACCESS) return void_t;

    AstNode* obj = node->ast_children[0];
    const char* field = node->ast_token.text;

    if (obj->ast_kind == AST_IDENT && strcmp(obj->ast_token.text, "this") == 0) {
        Type ft;
        if (class_find_field_type(cls, field, &ft)) return ft;
        return void_t;
    }

    Type ot = resolve_expr_type(obj, cls, scope);
    if (ot.type_kind == TYPE_CLASS && ot.type_arg_count == 0) {
        ClassInfo* ci = symtab_find_class(ot.class_name);
        Type ft;
        if (ci && class_find_field_type(ci, field, &ft)) return ft;
    }
    return void_t;
}

static Type resolve_expr_type(AstNode* node, ClassInfo* cls, LocalScope* scope) {
    Type void_t = {0};
    void_t.type_kind = TYPE_VOID;
    if (!node) return void_t;

    switch (node->ast_kind) {
        case AST_IDENT: {
            Type* t = local_scope_find(scope, node->ast_token.text);
            if (t) return *t;
            return void_t;
        }
        case AST_MEMBER_ACCESS:
            return resolve_member_type(node, cls, scope);
        case AST_AS_CAST:
            return node->ast_resolved_type;
        default:
            return void_t;
    }
}

static int validate_generic_calls_impl(AstNode* node, ClassInfo* cls, LocalScope* scope);

static int validate_generic_call(AstNode* node, ClassInfo* cls, LocalScope* scope) {
    if (!node) return 0;
    if (node->ast_kind != AST_CALL) return 0;
    if (node->ast_child_count == 0) return 0;
    AstNode* callee = node->ast_children[0];
    if (callee->ast_kind != AST_MEMBER_ACCESS) return 0;

    AstNode* mem = callee;
    AstNode* obj = mem->ast_children[0];
    const char* mname = mem->ast_token.text;

    Type ot = resolve_expr_type(obj, cls, scope);
    if (ot.type_kind == TYPE_TYPE_PARAM) {
        if (!type_param_has_interface_method(cls, ot.class_name, mname)) {
            fprintf(stderr, "error at %d:%d: type parameter '%s' has no interface constraint providing '%s()'\n",
                    mem->ast_token.line, mem->ast_token.col, ot.class_name, mname);
            return 1;
        }
    }
    return 0;
}

static int validate_generic_calls_impl(AstNode* node, ClassInfo* cls, LocalScope* scope) {
    if (!node) return 0;
    int errors = 0;

    errors += validate_generic_call(node, cls, scope);

    if (node->ast_kind == AST_VAR_DECL) {
        local_scope_push(scope, node->ast_token.text, node->ast_resolved_type);
    }

    int i;
    for (i = 0; i < node->ast_child_count && i < 4; i++) {
        errors += validate_generic_calls_impl(node->ast_children[i], cls, scope);
    }
    errors += validate_generic_calls_impl(node->next, cls, scope);
    return errors;
}

static int validate_generic_method_calls(ClassInfo* generic_def, AstNode* method) {
    LocalScope scope = {0};
    /* 'this' is not added; it is handled specially as a field access root. */

    /* parameters */
    if (method->ast_child_count > 0) {
        AstNode* p = method->ast_children[0];
        while (p) {
            if (p->ast_kind == AST_VAR_DECL) {
                local_scope_push(&scope, p->ast_token.text, p->ast_resolved_type);
            }
            p = p->next;
        }
    }

    int errors = 0;
    if (method->ast_child_count > 1) {
        errors += validate_generic_calls_impl(method->ast_children[1], generic_def, &scope);
    }
    return errors;
}

int symtab_validate_generic_method_calls(ClassInfo* generic_def) {
    if (!generic_def || !generic_def->is_generic || !generic_def->generic_ast) return 0;
    AstNode* class_node = generic_def->generic_ast;
    AstNode* methods = (class_node->ast_child_count > 0) ? class_node->ast_children[0] : NULL;
    int errors = 0;
    while (methods) {
        if (methods->ast_kind == AST_FUNC_DECL) {
            errors += validate_generic_method_calls(generic_def, methods);
        }
        methods = methods->next;
    }
    return errors;
}

ClassInfo* symtab_instantiate_class_from_type(Type* t) {
    if (t->type_kind != TYPE_CLASS || t->type_arg_count == 0) {
        return symtab_find_class(t->class_name);
    }

    ClassInfo* generic_def = symtab_find_class(t->class_name);
    if (!generic_def || !generic_def->is_generic) {
        fprintf(stderr, "error: type '%s' does not accept type arguments\n", t->class_name);
        return NULL;
    }

    const Type* args[MAX_TYPE_ARGS];
    int i;
    for (i = 0; i < t->type_arg_count && i < MAX_TYPE_ARGS; i++) {
        args[i] = t->type_args[i];
    }

    /* validate constraints */
    int p;
    for (p = 0; p < generic_def->generic_param_count && p < t->type_arg_count; p++) {
        int c;
        for (c = 0; c < generic_def->generic_constraint_count[p] && c < MAX_CONSTRAINTS_PER_PARAM; c++) {
            const char* iname = generic_def->generic_constraints[p][c];
            if (!type_implements_interface(args[p], iname)) {
                fprintf(stderr, "error: type argument '%s' does not implement interface '%s' required by '%s'\n",
                        type_name(args[p]), iname, generic_def->generic_params[p]);
                return NULL;
            }
        }
        if (generic_def->generic_has_new[p]) {
            if (args[p]->type_kind != TYPE_CLASS) {
                fprintf(stderr, "error: type argument '%s' for '%s' must be a class (new() constraint)\n",
                        type_name(args[p]), generic_def->generic_params[p]);
                return NULL;
            }
        }
    }

    /* Ensure nested generic class arguments are materialised so their
     * typedefs and method bodies are emitted before this instantiation. */
    int p2;
    for (p2 = 0; p2 < t->type_arg_count && p2 < MAX_TYPE_ARGS; p2++) {
        if (args[p2]->type_kind == TYPE_CLASS && args[p2]->type_arg_count > 0) {
            symtab_instantiate_class_from_type((Type*)args[p2]);
        }
    }

    ClassInfo* ci = symtab_add_class_instantiation(generic_def, args, t->type_arg_count);
    if (!ci) return NULL;

    /* build concrete field/method signatures and clone AST on first creation */
    if (!ci->field_count && !ci->methods && !ci->generic_ast) {
        const char* params[MAX_GENERIC_PARAMS];
        for (p = 0; p < generic_def->generic_param_count && p < MAX_GENERIC_PARAMS; p++) {
            params[p] = generic_def->generic_params[p];
        }

        ci->field_count = generic_def->field_count;
        for (i = 0; i < generic_def->field_count && i < MAX_FIELDS; i++) {
            CHECK_STRSCPY(strscpy(ci->field_names[i], generic_def->field_names[i], sizeof(ci->field_names[i])),
                          "field name too long");
            Type* ft = type_substitute(&generic_def->field_types[i], params, args, generic_def->generic_param_count);
            ci->field_types[i] = *ft;
            free(ft);
            type_mangled_name(&ci->field_types[i]);
        }

        MethodInfo* gm = generic_def->methods;
        MethodInfo* prev = NULL;
        while (gm) {
            MethodInfo* m = calloc(1, sizeof(MethodInfo));
            CHECK_STRSCPY(strscpy(m->name, gm->name, sizeof(m->name)), "method name too long");
            Type* rt = type_substitute(&gm->return_type, params, args, generic_def->generic_param_count);
            m->return_type = *rt;
            free(rt);
            type_mangled_name(&m->return_type);
            m->param_count = gm->param_count;
            int mp;
            for (mp = 0; mp < gm->param_count && mp < 16; mp++) {
                CHECK_STRSCPY(strscpy(m->param_names[mp], gm->param_names[mp], sizeof(m->param_names[mp])),
                              "parameter name too long");
                Type* pt = type_substitute(&gm->param_types[mp], params, args, generic_def->generic_param_count);
                m->param_types[mp] = *pt;
                free(pt);
                type_mangled_name(&m->param_types[mp]);
            }
            if (prev) prev->next = m; else ci->methods = m;
            prev = m;
            gm = gm->next;
        }

        ci->impl_count = generic_def->impl_count;
        for (i = 0; i < generic_def->impl_count && i < MAX_IMPL; i++) {
            CHECK_STRSCPY(strscpy(ci->impl_names[i], generic_def->impl_names[i], sizeof(ci->impl_names[i])),
                          "interface name too long");
            ci->impl_infos[i] = generic_def->impl_infos[i];
        }

        if (generic_def->generic_ast) {
            ci->generic_ast = ast_clone(generic_def->generic_ast);
            ast_substitute_types(ci->generic_ast, params, args, generic_def->generic_param_count);
            ci->generic_ast->ast_token.kind = TOK_IDENT;
            CHECK_STRSCPY(strscpy(ci->generic_ast->ast_token.text, ci->mangled_name,
                                  sizeof(ci->generic_ast->ast_token.text)),
                          "mangled class name too long");
        }
    }

    t->type_id = ci->type_id;
    return ci;
}
