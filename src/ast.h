#ifndef AST_H
#define AST_H

#include "token.h"

typedef enum {
    TYPE_VOID = 0,
    TYPE_CLASS,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_F32,
    TYPE_F64,
} TypeKind;

#define TYPE_ID_I8       0
#define TYPE_ID_I16      1
#define TYPE_ID_I32      2
#define TYPE_ID_I64      3
#define TYPE_ID_U8       4
#define TYPE_ID_U16      5
#define TYPE_ID_U32      6
#define TYPE_ID_U64      7
#define TYPE_ID_F32      8
#define TYPE_ID_F64      9
#define TYPE_ID_CLASS_BASE 16
#define TYPE_IS_ARRAY    0x80000000U

typedef struct {
    TypeKind kind;
    char     class_name[64];
    int      is_pointer;
    int      is_array;
    int      array_size;
    int      is_ref;       /* by-reference parameter */
    int      is_out;       /* out-parameter */
    int      is_in;        /* read-only by-reference parameter */
    int      type_id;
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
    AST_REF_ARG,
    AST_OUT_ARG,
} AstKind;

struct AstNode {
    AstKind  kind;
    Type     resolved_type;
    Token    tok;
    AstNode* children[4];
    int      child_count;
    char     temp_name[64];

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
