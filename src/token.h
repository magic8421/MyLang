#ifndef TOKEN_H
#define TOKEN_H

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_INT_LIT,
    TOK_CHAR_LIT,
    TOK_STRING_LIT,
    TOK_FLOAT_LIT,

    TOK_KW_U8,
    TOK_KW_U16,
    TOK_KW_U32,
    TOK_KW_U64,
    TOK_KW_I8,
    TOK_KW_I16,
    TOK_KW_I32,
    TOK_KW_I64,
    TOK_KW_F32,
    TOK_KW_F64,
    TOK_KW_REF,
    TOK_KW_WEAK,
    TOK_KW_STRUCT,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_FOR,
    TOK_KW_RETURN,
    TOK_KW_NEW,
    TOK_KW_CLASS,
    TOK_KW_THIS,
    TOK_KW_INTERFACE,
    TOK_KW_AS,
    TOK_KW_NATIVE,
    TOK_KW_STRING,

    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_ASSIGN,
    TOK_PLUS_ASSIGN,
    TOK_MINUS_ASSIGN,
    TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN,
    TOK_INC,
    TOK_DEC,
    TOK_NOT,
    TOK_AND,
    TOK_OR,

    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_SEMI,
    TOK_COMMA,
    TOK_DOT,
    TOK_COLON,
} TokenKind;

typedef struct {
    TokenKind kind;
    char text[256];
    int  line;
    int  col;
    int  int_val;
    char char_val;
    float float_val;
} Token;

const char* token_kind_name(TokenKind kind);

#endif
