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
    char* buf = calloc(1, size + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static void derive_header_path(const char* out_path,
                               char* header_path, size_t header_path_size,
                               char* header_name, size_t header_name_size) {
    const char* base = out_path;
    const char* last_sep_back = strrchr(out_path, '\\');
    const char* last_sep_fwd  = strrchr(out_path, '/');
    const char* last_sep = last_sep_back > last_sep_fwd ? last_sep_back : last_sep_fwd;
    if (last_sep) base = last_sep + 1;

    const char* dot = strrchr(base, '.');
    size_t name_len = dot ? (size_t)(dot - base) : strlen(base);
    size_t dir_len = (size_t)(base - out_path);

    if (header_path) {
        int n = snprintf(header_path, header_path_size, "%.*s%.*s.h",
                         (int)dir_len, out_path, (int)name_len, base);
        if (n < 0 || (size_t)n >= header_path_size) {
            header_path[header_path_size - 1] = '\0';
        }
    }
    if (header_name) {
        int n = snprintf(header_name, header_name_size, "%.*s.h",
                         (int)name_len, base);
        if (n < 0 || (size_t)n >= header_name_size) {
            header_name[header_name_size - 1] = '\0';
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mylang [--leak-check] <source.my> [output.c]\n");
        return 1;
    }

    const char* src_path = NULL;
    const char* out_path = "out.c";
    int leak_check = 0;
    int xor_strings = 0;

    int argi;
    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--leak-check") == 0) {
            leak_check = 1;
        } else if (strcmp(argv[argi], "--xor-strings") == 0) {
            xor_strings = 1;
        } else if (!src_path) {
            src_path = argv[argi];
        } else {
            out_path = argv[argi];
        }
    }

    if (!src_path) {
        fprintf(stderr, "usage: mylang [--leak-check] [--xor-strings] <source.my> [output.c]\n");
        return 1;
    }

    char* source = read_file(src_path);
    if (!source) return 1;

    Lexer lexer;
    lexer_init(&lexer, source);
    lexer_set_filename(&lexer, src_path);

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

    char header_path[1024];
    char header_name[256];
    derive_header_path(out_path, header_path, sizeof(header_path),
                       header_name, sizeof(header_name));

    FILE* header = fopen(header_path, "w");
    if (!header) {
        fprintf(stderr, "error: cannot write header '%s'\n", header_path);
        fclose(out);
        free(source);
        return 1;
    }

    codegen_program(ast, out, header, src_path, leak_check, header_name, xor_strings);
    fclose(out);
    fclose(header);

    if (codegen_had_error()) {
        free(source);
        return 1;
    }

    printf("compiled '%s' -> '%s', '%s'\n", src_path, out_path, header_path);

    free(source);
    return 0;
}
