#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

AstNode* ast_new_node(AstKind kind, Token tok) {
    AstNode* node = calloc(1, sizeof(AstNode));
    node->kind = kind;
    node->tok  = tok;
    return node;
}

void ast_add_child(AstNode* parent, AstNode* child) {
    if (parent->child_count < 4) {
        parent->children[parent->child_count++] = child;
    } else {
        fprintf(stderr, "error: too many children for AST node kind=%d\n", parent->kind);
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
    static char buf[128];
    switch (t->kind) {
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
        case TYPE_VOID:  return "void";
        case TYPE_CLASS: return t->class_name;
    }
    return "?";
}

int type_equal(const Type* a, const Type* b) {
    if (a->kind != b->kind) return 0;
    if (a->is_pointer != b->is_pointer) return 0;
    if (a->is_array != b->is_array) return 0;
    if (a->array_size != b->array_size) return 0;
    if (a->is_ref != b->is_ref) return 0;
    if (a->kind == TYPE_CLASS || a->kind == TYPE_STRUCT) {
        return strcmp(a->class_name, b->class_name) == 0;
    }
    return 1;
}
