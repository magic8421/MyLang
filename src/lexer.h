#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    const char* source;
    int         pos;
    int         line;
    int         col;
    const char* filename;
} Lexer;

void  lexer_init(Lexer* lexer, const char* source);
void  lexer_set_filename(Lexer* lexer, const char* filename);
const char* lexer_filename(const Lexer* lexer);
Token lexer_next(Lexer* lexer);

#endif
