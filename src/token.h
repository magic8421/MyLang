#ifndef TOKEN_H
#define TOKEN_H

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_INT_LIT,
    TOK_CHAR_LIT,

    TOK_KW_INT,
    TOK_KW_CHAR,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_RETURN,
    TOK_KW_NEW,
    TOK_KW_CLASS,

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
} TokenKind;

typedef struct {
    TokenKind kind;
    char text[256];
    int  line;
    int  col;
    int  int_val;
    char char_val;
} Token;

const char* token_kind_name(TokenKind kind);

#endif
