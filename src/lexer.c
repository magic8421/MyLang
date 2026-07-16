#include "lexer.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    const char* keyword;
    TokenKind   kind;
} KeywordEntry;

static const KeywordEntry keywords[] = {
    {"u8",     TOK_KW_U8},
    {"u16",    TOK_KW_U16},
    {"u32",    TOK_KW_U32},
    {"u64",    TOK_KW_U64},
    {"i8",     TOK_KW_I8},
    {"i16",    TOK_KW_I16},
    {"i32",    TOK_KW_I32},
    {"i64",    TOK_KW_I64},
    {"f32",    TOK_KW_F32},
    {"f64",    TOK_KW_F64},
    {"ref",    TOK_KW_REF},
    {"weak",   TOK_KW_WEAK},
    {"struct", TOK_KW_STRUCT},
    {"if",     TOK_KW_IF},
    {"else",   TOK_KW_ELSE},
    {"while",  TOK_KW_WHILE},
    {"for",    TOK_KW_FOR},
    {"return", TOK_KW_RETURN},
    {"new",    TOK_KW_NEW},
    {"class",     TOK_KW_CLASS},
    {"this",      TOK_KW_THIS},
    {"interface", TOK_KW_INTERFACE},
    {"as",        TOK_KW_AS},
    {"native",    TOK_KW_NATIVE},
    {NULL,        0},
};

void lexer_init(Lexer* lexer, const char* source) {
    lexer->source = source;
    lexer->pos    = 0;
    lexer->line   = 1;
    lexer->col    = 1;
}

static char lexer_peek_char(Lexer* lexer) {
    return lexer->source[lexer->pos];
}

static char lexer_peek_ahead(Lexer* lexer, int offset) {
    return lexer->source[lexer->pos + offset];
}

static int lexer_is_eof(Lexer* lexer) {
    return lexer->source[lexer->pos] == '\0';
}

static void lexer_advance(Lexer* lexer) {
    if (lexer->source[lexer->pos] == '\n') {
        lexer->line++;
        lexer->col = 1;
    } else {
        lexer->col++;
    }
    lexer->pos++;
}

static void skip_whitespace_and_comments(Lexer* lexer) {
    for (;;) {
        if (lexer_is_eof(lexer)) return;
        char c = lexer_peek_char(lexer);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            lexer_advance(lexer);
        } else if (c == '/' && lexer_peek_ahead(lexer, 1) == '/') {
            while (!lexer_is_eof(lexer) && lexer_peek_char(lexer) != '\n') {
                lexer_advance(lexer);
            }
        } else {
            break;
        }
    }
}

static TokenKind lookup_keyword(const char* text) {
    for (int i = 0; keywords[i].keyword != NULL; i++) {
        if (strcmp(text, keywords[i].keyword) == 0) {
            return keywords[i].kind;
        }
    }
    return TOK_IDENT;
}

static Token make_token(Lexer* lexer, TokenKind kind, const char* text, int length, int col_offset) {
    Token tok;
    tok.kind     = kind;
    tok.line     = lexer->line;
    tok.col      = lexer->col - col_offset;
    tok.int_val  = 0;
    tok.char_val = 0;
    if (text && length > 0) {
        int n = length < 255 ? length : 255;
        memcpy(tok.text, text, n);
        tok.text[n] = '\0';
    } else {
        tok.text[0] = '\0';
    }
    return tok;
}

static Token read_identifier(Lexer* lexer) {
    int start = lexer->pos;
    while (!lexer_is_eof(lexer)) {
        char c = lexer_peek_char(lexer);
        if (isalnum((unsigned char)c) || c == '_') {
            lexer_advance(lexer);
        } else {
            break;
        }
    }
    int len = lexer->pos - start;
    const char* s = lexer->source + start;
    Token tok = make_token(lexer, TOK_IDENT, s, len, len);
    tok.kind = lookup_keyword(tok.text);
    return tok;
}

static Token read_number(Lexer* lexer) {
    int start = lexer->pos;
    while (!lexer_is_eof(lexer) && isdigit((unsigned char)lexer_peek_char(lexer))) {
        lexer_advance(lexer);
    }
    int len = lexer->pos - start;
    const char* s = lexer->source + start;
    Token tok = make_token(lexer, TOK_INT_LIT, s, len, len);
    tok.int_val = atoi(tok.text);
    return tok;
}

static Token read_char_literal(Lexer* lexer) {
    lexer_advance(lexer); /* skip opening ' */

    if (lexer_is_eof(lexer)) {
        fprintf(stderr, "lexer error at %d:%d: unterminated char literal\n",
                lexer->line, lexer->col);
        return make_token(lexer, TOK_CHAR_LIT, NULL, 0, 0);
    }

    char c = lexer_peek_char(lexer);
    if (c == '\\') {
        lexer_advance(lexer);
        if (lexer_is_eof(lexer)) {
            fprintf(stderr, "lexer error at %d:%d: unterminated char literal after '\\'\n",
                    lexer->line, lexer->col);
            return make_token(lexer, TOK_CHAR_LIT, NULL, 0, 0);
        }
        c = lexer_peek_char(lexer);
        switch (c) {
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '0':  c = '\0'; break;
            default:   break;
        }
        lexer_advance(lexer);
    } else {
        lexer_advance(lexer);
    }
    if (!lexer_is_eof(lexer) && lexer_peek_char(lexer) == '\'') {
        lexer_advance(lexer); /* skip closing ' */
    }
    Token tok = make_token(lexer, TOK_CHAR_LIT, NULL, 0, 0);
    tok.char_val = c;
    tok.text[0] = c;
    tok.text[1] = '\0';
    return tok;
}

Token lexer_next(Lexer* lexer) {
    skip_whitespace_and_comments(lexer);

    if (lexer_is_eof(lexer)) {
        return make_token(lexer, TOK_EOF, NULL, 0, 0);
    }

    int start_line = lexer->line;
    int start_col  = lexer->col;

    char c = lexer_peek_char(lexer);

    if (isalpha((unsigned char)c) || c == '_') {
        return read_identifier(lexer);
    }

    if (isdigit((unsigned char)c)) {
        return read_number(lexer);
    }

    if (c == '\'') {
        return read_char_literal(lexer);
    }

    /* two-character operators */
    if (c == '=' && lexer_peek_ahead(lexer, 1) == '=') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_EQ, "==", 2, 2);
    }
    if (c == '!' && lexer_peek_ahead(lexer, 1) == '=') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_NE, "!=", 2, 2);
    }
    if (c == '<' && lexer_peek_ahead(lexer, 1) == '=') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_LE, "<=", 2, 2);
    }
    if (c == '>' && lexer_peek_ahead(lexer, 1) == '=') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_GE, ">=", 2, 2);
    }
    if (c == '&' && lexer_peek_ahead(lexer, 1) == '&') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_AND, "&&", 2, 2);
    }
    if (c == '|' && lexer_peek_ahead(lexer, 1) == '|') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_OR, "||", 2, 2);
    }
    if (c == '+' && lexer_peek_ahead(lexer, 1) == '=') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_PLUS_ASSIGN, "+=", 2, 2);
    }
    if (c == '-' && lexer_peek_ahead(lexer, 1) == '=') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_MINUS_ASSIGN, "-=", 2, 2);
    }
    if (c == '*' && lexer_peek_ahead(lexer, 1) == '=') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_STAR_ASSIGN, "*=", 2, 2);
    }
    if (c == '/' && lexer_peek_ahead(lexer, 1) == '=') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_SLASH_ASSIGN, "/=", 2, 2);
    }
    if (c == '+' && lexer_peek_ahead(lexer, 1) == '+') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_INC, "++", 2, 2);
    }
    if (c == '-' && lexer_peek_ahead(lexer, 1) == '-') {
        lexer_advance(lexer); lexer_advance(lexer);
        return make_token(lexer, TOK_DEC, "--", 2, 2);
    }

    /* single-character tokens */
    lexer_advance(lexer);
    switch (c) {
        case '+': return make_token(lexer, TOK_PLUS,     "+", 1, 1);
        case '-': return make_token(lexer, TOK_MINUS,    "-", 1, 1);
        case '*': return make_token(lexer, TOK_STAR,     "*", 1, 1);
        case '/': return make_token(lexer, TOK_SLASH,    "/", 1, 1);
        case '%': return make_token(lexer, TOK_PERCENT,  "%", 1, 1);
        case '<': return make_token(lexer, TOK_LT,       "<", 1, 1);
        case '>': return make_token(lexer, TOK_GT,       ">", 1, 1);
        case '=': return make_token(lexer, TOK_ASSIGN,   "=", 1, 1);
        case '!': return make_token(lexer, TOK_NOT,      "!", 1, 1);
        case '(': return make_token(lexer, TOK_LPAREN,   "(", 1, 1);
        case ')': return make_token(lexer, TOK_RPAREN,   ")", 1, 1);
        case '{': return make_token(lexer, TOK_LBRACE,   "{", 1, 1);
        case '}': return make_token(lexer, TOK_RBRACE,   "}", 1, 1);
        case '[': return make_token(lexer, TOK_LBRACKET, "[", 1, 1);
        case ']': return make_token(lexer, TOK_RBRACKET, "]", 1, 1);
        case ';': return make_token(lexer, TOK_SEMI,     ";", 1, 1);
        case ',': return make_token(lexer, TOK_COMMA,    ",", 1, 1);
        case '.': return make_token(lexer, TOK_DOT,      ".", 1, 1);
        case ':': return make_token(lexer, TOK_COLON,    ":", 1, 1);
        default: {
            fprintf(stderr, "lexer error at %d:%d: unexpected character '%c' (0x%02X)\n",
                    lexer->line, lexer->col, c, (unsigned char)c);
            return make_token(lexer, TOK_EOF, NULL, 0, 0);
        }
    }
}
