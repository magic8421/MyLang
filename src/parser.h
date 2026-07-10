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
} Parser;

void     parser_init(Parser* p, Lexer* lexer);
AstNode* parser_parse_program(Parser* p);
int      parser_had_error(Parser* p);

#endif
