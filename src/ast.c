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
        case TYPE_INT:    return "int";
        case TYPE_CHAR:   return "char";
        case TYPE_VOID:   return "void";
        case TYPE_STRUCT: return t->struct_name;
    }
    return "?";
}

int type_equal(const Type* a, const Type* b) {
    if (a->kind != b->kind) return 0;
    if (a->is_pointer != b->is_pointer) return 0;
    if (a->kind == TYPE_STRUCT) {
        return strcmp(a->struct_name, b->struct_name) == 0;
    }
    return 1;
}
