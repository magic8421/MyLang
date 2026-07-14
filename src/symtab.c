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

ClassInfo* symtab_add_class_instantiation(ClassInfo* generic_def, const Type* args, int arg_count) {
    if (arg_count != generic_def->generic_param_count) {
        fprintf(stderr, "error: generic class '%s' expects %d type arguments, got %d\n",
                generic_def->name, generic_def->generic_param_count, arg_count);
        return NULL;
    }

    Type inst_type = type_make_user(TYPE_CLASS, generic_def->name);
    int i;
    for (i = 0; i < arg_count && i < MAX_TYPE_ARGS; i++) {
        type_set_arg(&inst_type, i, &args[i]);
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
    CHECK_STRSCPY(strscpy(ci->name, generic_def->name, sizeof(ci->name)), "class name too long");
    CHECK_STRSCPY(strscpy(ci->mangled_name, mangled, sizeof(ci->mangled_name)), "mangled class name too long");
    ci->type_id = symtab_next_type_id();
    ci->is_instantiation = 1;
    ci->generic_def = generic_def;
    ci->instantiation_arg_count = arg_count;
    for (i = 0; i < arg_count && i < MAX_GENERIC_PARAMS; i++) {
        ci->instantiation_args[i] = *type_new(&args[i]);
    }

    for (i = 0; i < inst_type.type_arg_count && i < MAX_TYPE_ARGS; i++) {
        type_free(inst_type.type_args[i]);
    }

    ci->next = class_list;
    class_list = ci;
    return ci;
}
