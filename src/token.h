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
    TOK_KW_UNOWNED,
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
    TOK_KW_OVERRIDE,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
    TOK_KW_MATCH,
    TOK_KW_NULL,
    TOK_KW_BOOL,
    TOK_KW_TRUE,
    TOK_KW_FALSE,

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
    TOK_FATARROW,

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

/* Sentinel bytes used inside string-literal token text: the lexer maps the
   escapes '\{' and '\}' to these so the f-string parser can tell escaped
   braces apart from real interpolation braces.  The parser converts them
   back to '{' and '}' before the text reaches codegen. */
#define TOK_ESC_LBRACE '\x01'
#define TOK_ESC_RBRACE '\x02'

const char* token_kind_name(TokenKind kind);

#endif
