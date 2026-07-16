#ifndef AST_H
#define AST_H

#include "token.h"

typedef enum {
    TYPE_VOID = 0,
    TYPE_CLASS,
    TYPE_STRUCT,
    TYPE_INTERFACE,
    TYPE_TYPE_PARAM,
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
#define TYPE_ID_STRING          10
#define TYPE_ID_CLASS_BASE      16
#define TYPE_IS_ARRAY    0x80000000U
#define TYPE_IS_STRUCT   0x40000000U
#define TYPE_IS_WEAK     0x20000000U
#define TYPE_IS_INTERFACE 0x10000000U

#define MAX_TYPE_ARGS 4

typedef struct Type {
    TypeKind type_kind;
    char     class_name[64];
    int      is_pointer;
    int      is_array;
    int      array_size;
    int      is_ref;       /* by-reference parameter */
    int      is_weak;      /* weak reference */
    int      type_id;

    int      type_arg_count;
    struct Type* type_args[MAX_TYPE_ARGS];
    char     mangled_name[128];
} Type;

typedef struct AstNode AstNode;

typedef enum {
    AST_PROGRAM,
    AST_CLASS_DECL,
    AST_STRUCT_DECL,
    AST_INTERFACE_DECL,
    AST_FUNC_DECL,
    AST_BLOCK,
    AST_VAR_DECL,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_FOR_STMT,
    AST_RETURN_STMT,
    AST_BREAK,
    AST_CONTINUE,
    AST_MATCH,
    AST_MATCH_ARM,
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
    AST_STRING_LIT,
    AST_FLOAT_LIT,
    AST_AS_CAST,
    AST_REF_ARG,
    AST_OUT_ARG,
    AST_INC_DEC,
    AST_FSTRING,
} AstKind;

struct AstNode {
    AstKind  ast_kind;
    Type     ast_resolved_type;
    Token    ast_token;
    AstNode* ast_children[4];
    int      ast_child_count;
    char     ast_temp_name[64];
    char     ast_match_var[64];
    int      ast_is_native;

    /* for AST_BINARY / AST_UNARY: op stored in tok.kind */
    /* for AST_IDENT / AST_CLASS_DECL: name in tok.text */
    AstNode* next;  /* linked list for decls / stmts */
};

AstNode* ast_new_node(AstKind kind, Token tok);
void     ast_add_child(AstNode* parent, AstNode* child);
AstNode* ast_append_list(AstNode* head, AstNode* item);

const char* type_name(const Type* t);
int         type_equal(const Type* a, const Type* b);

/* Type construction helpers */
Type        type_make_primitive(TypeKind kind);
Type        type_make_user(TypeKind kind, const char* name);
Type        type_make_param(const char* name);
Type*       type_new(const Type* src);
void        type_free(Type* t);
int         type_is_param(const Type* t);
int         type_is_generic(const Type* t);
const char* type_mangled_name(Type* t);
Type*       type_substitute(const Type* t, const char* params[], const Type* args[], int count);
void        type_set_arg(Type* t, int idx, const Type* arg);
void        type_free_args(Type* t);

AstNode*    ast_clone(AstNode* node);
void        ast_substitute_types(AstNode* node, const char* params[], const Type* args[], int count);

#endif
