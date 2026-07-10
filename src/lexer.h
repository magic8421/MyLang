#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    const char* source;
    int         pos;
    int         line;
    int         col;
} Lexer;

void  lexer_init(Lexer* lexer, const char* source);
Token lexer_next(Lexer* lexer);

#endif
