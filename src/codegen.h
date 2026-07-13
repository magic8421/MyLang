#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include <stdio.h>

void codegen_program(AstNode* program, FILE* out, const char* source_file, int leak_check);

#endif
