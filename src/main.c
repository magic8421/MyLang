#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mylang [--leak-check] <source.my> [output.c]\n");
        return 1;
    }

    const char* src_path = NULL;
    const char* out_path = "out.c";
    int leak_check = 0;

    int argi;
    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--leak-check") == 0) {
            leak_check = 1;
        } else if (!src_path) {
            src_path = argv[argi];
        } else {
            out_path = argv[argi];
        }
    }

    if (!src_path) {
        fprintf(stderr, "usage: mylang [--leak-check] <source.my> [output.c]\n");
        return 1;
    }

    char* source = read_file(src_path);
    if (!source) return 1;

    Lexer lexer;
    lexer_init(&lexer, source);

    Parser parser;
    parser_init(&parser, &lexer);

    AstNode* ast = parser_parse_program(&parser);

    if (parser_had_error(&parser)) {
        free(source);
        return 1;
    }

    FILE* out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "error: cannot write '%s'\n", out_path);
        free(source);
        return 1;
    }

    codegen_program(ast, out, src_path, leak_check);
    fclose(out);

    printf("compiled '%s' -> '%s'\n", src_path, out_path);

    free(source);
    return 0;
}
