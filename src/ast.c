#include "ast.h"
#include "mangle.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

AstNode* ast_new_node(AstKind kind, Token tok) {
    AstNode* node = calloc(1, sizeof(AstNode));
    node->ast_kind = kind;
    node->ast_token  = tok;
    return node;
}

void ast_add_child(AstNode* parent, AstNode* child) {
    if (parent->ast_child_count < MAX_AST_CHILDREN) {
        parent->ast_children[parent->ast_child_count++] = child;
    } else {
        fprintf(stderr, "error: too many children for AST node kind=%d\n", parent->ast_kind);
    }
}

AstNode* ast_append_list(AstNode* head, AstNode* item) {
    if (!head) return item;
    AstNode* cur = head;
    while (cur->next) cur = cur->next;
    cur->next = item;
    return head;
}

const char* type_name(const Type* t) {
    switch (t->type_kind) {
        case TYPE_I8:    return "i8";
        case TYPE_I16:   return "i16";
        case TYPE_I32:   return "i32";
        case TYPE_I64:   return "i64";
        case TYPE_U8:    return "u8";
        case TYPE_U16:   return "u16";
        case TYPE_U32:   return "u32";
        case TYPE_U64:   return "u64";
        case TYPE_F32:   return "f32";
        case TYPE_F64:   return "f64";
        case TYPE_BOOL:  return "bool";
        case TYPE_OBJECT: return "object";
        case TYPE_NULL:  return "null";
        case TYPE_VOID:  return "void";
        case TYPE_TYPE_PARAM: return t->class_name;
        case TYPE_CLASS:
        case TYPE_STRUCT:
        case TYPE_INTERFACE:
            if (t->type_arg_count > 0) return type_mangled_name((Type*)t);
            return t->class_name;
    }
    return "?";
}

int type_equal(const Type* a, const Type* b) {
    if (a->type_kind != b->type_kind) return 0;
    if (a->is_pointer != b->is_pointer) return 0;
    if (a->is_array != b->is_array) return 0;
    if (a->array_size != b->array_size) return 0;
    if (a->is_ref != b->is_ref) return 0;
    if (a->is_weak != b->is_weak) return 0;
    if (a->is_unowned != b->is_unowned) return 0;
    if (a->type_arg_count != b->type_arg_count) return 0;
    if (a->type_kind == TYPE_CLASS || a->type_kind == TYPE_STRUCT ||
        a->type_kind == TYPE_INTERFACE || a->type_kind == TYPE_TYPE_PARAM) {
        if (strcmp(a->class_name, b->class_name) != 0) return 0;
    }
    int i;
    for (i = 0; i < a->type_arg_count && i < MAX_TYPE_ARGS; i++) {
        if (!a->type_args[i] || !b->type_args[i]) return 0;
        if (!type_equal(a->type_args[i], b->type_args[i])) return 0;
    }
    return 1;
}

Type type_make_primitive(TypeKind kind) {
    Type t = {0};
    t.type_kind = kind;
    switch (kind) {
        case TYPE_I8:  t.type_id = TYPE_ID_I8;  break;
        case TYPE_I16: t.type_id = TYPE_ID_I16; break;
        case TYPE_I32: t.type_id = TYPE_ID_I32; break;
        case TYPE_I64: t.type_id = TYPE_ID_I64; break;
        case TYPE_U8:  t.type_id = TYPE_ID_U8;  break;
        case TYPE_U16: t.type_id = TYPE_ID_U16; break;
        case TYPE_U32: t.type_id = TYPE_ID_U32; break;
        case TYPE_U64: t.type_id = TYPE_ID_U64; break;
        case TYPE_F32: t.type_id = TYPE_ID_F32; break;
        case TYPE_F64: t.type_id = TYPE_ID_F64; break;
        case TYPE_BOOL: t.type_id = TYPE_ID_BOOL; break;
        default: break;
    }
    return t;
}

Type type_make_user(TypeKind kind, const char* name) {
    Type t = {0};
    t.type_kind = kind;
    CHECK_STRSCPY(strscpy(t.class_name, name, sizeof(t.class_name)), "user type name too long");
    return t;
}

Type type_make_param(const char* name) {
    Type t = {0};
    t.type_kind = TYPE_TYPE_PARAM;
    CHECK_STRSCPY(strscpy(t.class_name, name, sizeof(t.class_name)), "type param name too long");
    return t;
}

Type* type_new(const Type* src) {
    Type* p = malloc(sizeof(Type));
    *p = *src;
    p->mangled_name[0] = '\0';
    int i;
    for (i = 0; i < MAX_TYPE_ARGS; i++) p->type_args[i] = NULL;
    p->type_arg_count = 0;
    for (i = 0; i < src->type_arg_count && i < MAX_TYPE_ARGS; i++) {
        p->type_args[i] = type_new(src->type_args[i]);
        p->type_arg_count++;
    }
    return p;
}

void type_free(Type* t) {
    if (!t) return;
    int i;
    for (i = 0; i < t->type_arg_count && i < MAX_TYPE_ARGS; i++) {
        type_free(t->type_args[i]);
    }
    free(t);
}

void type_free_args(Type* t) {
    if (!t) return;
    int i;
    for (i = 0; i < t->type_arg_count && i < MAX_TYPE_ARGS; i++) {
        type_free(t->type_args[i]);
        t->type_args[i] = NULL;
    }
    t->type_arg_count = 0;
    t->mangled_name[0] = '\0';
}

int type_is_param(const Type* t) {
    return t->type_kind == TYPE_TYPE_PARAM;
}

int type_is_generic(const Type* t) {
    return t->type_arg_count > 0;
}

const char* type_mangled_name(Type* t) {
    return mangle_type(t);
}

Type* type_substitute(const Type* t, const char* params[], const Type* args[], int count) {
    int i;
    for (i = 0; i < count; i++) {
        if (t->type_kind == TYPE_TYPE_PARAM && strcmp(t->class_name, params[i]) == 0) {
            return type_new(args[i]);
        }
    }
    Type* r = type_new(t);
    for (i = 0; i < r->type_arg_count && i < MAX_TYPE_ARGS; i++) {
        Type* sub = type_substitute(r->type_args[i], params, args, count);
        type_free(r->type_args[i]);
        r->type_args[i] = sub;
    }
    r->mangled_name[0] = '\0';
    type_mangled_name(r);
    return r;
}

void type_set_arg(Type* t, int idx, const Type* arg) {
    if (idx < 0 || idx >= MAX_TYPE_ARGS) return;
    if (t->type_args[idx]) {
        type_free(t->type_args[idx]);
    }
    t->type_args[idx] = type_new(arg);
    if (idx >= t->type_arg_count) {
        t->type_arg_count = idx + 1;
    }
}

AstNode* ast_clone(AstNode* node) {
    if (!node) return NULL;
    AstNode* copy = ast_new_node(node->ast_kind, node->ast_token);
    copy->ast_resolved_type = *type_new(&node->ast_resolved_type);
    copy->ast_child_count = node->ast_child_count;
    int i;
    for (i = 0; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
        copy->ast_children[i] = ast_clone(node->ast_children[i]);
    }
    copy->next = ast_clone(node->next);
    return copy;
}

static void ast_substitute_types_node(AstNode* node, const char* params[], const Type* args[], int count) {
    if (!node) return;
    Type* sub = type_substitute(&node->ast_resolved_type, params, args, count);
    type_free_args(&node->ast_resolved_type);
    node->ast_resolved_type = *sub;
    free(sub);
    int i;
    for (i = 0; i < node->ast_child_count && i < MAX_AST_CHILDREN; i++) {
        ast_substitute_types_node(node->ast_children[i], params, args, count);
    }
    ast_substitute_types_node(node->next, params, args, count);
}

void ast_substitute_types(AstNode* node, const char* params[], const Type* args[], int count) {
    ast_substitute_types_node(node, params, args, count);
}
