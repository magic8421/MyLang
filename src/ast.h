#ifndef AST_H
#define AST_H

#include "token.h"

typedef enum {
    TYPE_INT,
    TYPE_CHAR,
    TYPE_CLASS,
    TYPE_VOID,
} TypeKind;

typedef struct {
    TypeKind kind;
    char     class_name[64];
    int      is_pointer;
    int      array_size;
} Type;

typedef struct AstNode AstNode;

typedef enum {
    AST_PROGRAM,
    AST_CLASS_DECL,
    AST_FUNC_DECL,
    AST_BLOCK,
    AST_VAR_DECL,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_RETURN_STMT,
    AST_EXPR_STMT,
    AST_ASSIGN,
    AST_BINARY,
    AST_UNARY,
    AST_CALL,
    AST_NEW,
    AST_ARRAY_ACCESS,
    AST_MEMBER_ACCESS,
    AST_IDENT,
    AST_INT_LIT,
    AST_CHAR_LIT,
} AstKind;

struct AstNode {
    AstKind  kind;
    Type     resolved_type;
    Token    tok;
    AstNode* children[4];
    int      child_count;
    char     temp_name[32];

    /* for AST_BINARY / AST_UNARY: op stored in tok.kind */
    /* for AST_IDENT / AST_CLASS_DECL: name in tok.text */
    AstNode* next;  /* linked list for decls / stmts */
};

AstNode* ast_new_node(AstKind kind, Token tok);
void     ast_add_child(AstNode* parent, AstNode* child);
AstNode* ast_append_list(AstNode* head, AstNode* item);

const char* type_name(const Type* t);
int         type_equal(const Type* a, const Type* b);

#endif
