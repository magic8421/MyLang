#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include <stdio.h>

void codegen_program(AstNode* program, FILE* out, FILE* header,
                     const char* source_file, int leak_check,
                     const char* header_include_name, int xor_strings);
int codegen_had_error(void);

#endif
