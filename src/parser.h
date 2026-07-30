#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Lexer*  lexer;
    Token   current;
    Token   peek;
    Token   peek2;
    int     had_error;
    /* Name of the enclosing namespace block while parsing one ("" outside).
       Declaration names read under it are qualified to "N_name" at parse
       time, so downstream passes only ever see the underscored form. */
    char    ns_prefix[NAME_BUF_SIZE];
} Parser;

void     parser_init(Parser* p, Lexer* lexer);
AstNode* parser_parse_program(Parser* p);
int      parser_had_error(Parser* p);

#endif
