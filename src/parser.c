#include "parser.h"
#include "symtab.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char* parser_filename(Parser* p) {
    return lexer_filename(p->lexer);
}

/* ================================================================
   LEXER HELPERS
   ================================================================ */

static void advance(Parser* p) {
    p->current = p->peek;
    p->peek    = p->peek2;
    p->peek2   = lexer_next(p->lexer);
}

static int check(Parser* p, TokenKind k)      { return p->current.kind == k; }

static int expect(Parser* p, TokenKind k) {
    if (check(p, k)) { advance(p); return 1; }
    fprintf(stderr, "%s(%d,%d): error: expected '%s', got '%s'\n",
            parser_filename(p), p->current.line, p->current.col,
            token_kind_name(k), token_kind_name(p->current.kind));
    p->had_error = 1;
    return 0;
}

static int expr_contains_assign(AstNode* node) {
    if (!node) return 0;
    if (node->ast_kind == AST_ASSIGN) return 1;
    int i;
    for (i = 0; i < node->ast_child_count; i++) {
        if (expr_contains_assign(node->ast_children[i])) return 1;
    }
    return expr_contains_assign(node->next);
}

static int expr_is_direct_assignment(AstNode* node) {
    return node && node->ast_kind == AST_ASSIGN;
}

static int expr_is_inc_dec(AstNode* node) {
    return node && node->ast_kind == AST_INC_DEC;
}

static int expr_contains_inc_dec(AstNode* node) {
    if (!node) return 0;
    if (node->ast_kind == AST_INC_DEC) return 1;
    int i;
    for (i = 0; i < node->ast_child_count; i++) {
        if (expr_contains_inc_dec(node->ast_children[i])) return 1;
    }
    return expr_contains_inc_dec(node->next);
}

static int parser_is_type_param(const char* name);  /* defined below */

static int is_type_name(const char* name) {
    return parser_is_type_param(name) ||
           symtab_find_class(name) != NULL ||
           symtab_find_struct(name) != NULL ||
           symtab_find_interface(name) != NULL ||
           symtab_find_enum(name) != NULL;
}

/* Rewrite a declaration name token to its namespace-qualified underscored
   form ("N_name") when the parser is inside a namespace block.  Everything
   downstream (symtab, sema, codegen) only ever sees the qualified form, so
   C emission needs no namespace awareness. */
static void parser_qualify_decl_name(Parser* p, Token* name) {
    if (!p->ns_prefix[0]) return;
    char q[TOKEN_TEXT_SIZE];
    int n = snprintf(q, sizeof(q), "%s_%s", p->ns_prefix, name->text);
    CHECK_SNPRINTF(n, sizeof(q), "qualified name too long");
    CHECK_STRSCPY(strscpy(name->text, q, sizeof(name->text)), "qualified name too long");
}

/* ================================================================
   FORWARD DECLARATIONS
   ================================================================ */

static AstNode* parse_stmt(Parser* p);
static AstNode* parse_match_stmt(Parser* p);
static AstNode* parse_expr(Parser* p);
static AstNode* parse_expr_no_assign(Parser* p, const char* where);
static AstNode* parse_required_block(Parser* p, const char* what, int allow_if);
static AstNode* parse_namespace_decl(Parser* p);
static AstNode* parse_top_level(Parser* p);
static void parse_const_initializer(Parser* p, Token name, const Type* t,
                                    int is_string, ConstInfo* info);



static const char* parser_type_params[MAX_GENERIC_PARAMS];
static int         parser_type_param_count = 0;

static void parser_push_type_params(ClassInfo* info) {
    int i;
    parser_type_param_count = 0;
    for (i = 0; i < info->generic_param_count && i < MAX_GENERIC_PARAMS; i++) {
        parser_type_params[i] = info->generic_params[i];
    }
    parser_type_param_count = i;
}

static void parser_pop_type_params(void) {
    parser_type_param_count = 0;
}

static int parser_is_type_param(const char* name) {
    int i;
    for (i = 0; i < parser_type_param_count; i++) {
        if (strcmp(parser_type_params[i], name) == 0) return 1;
    }
    return 0;
}

/* Close a generic type-argument list.  The lexer produces '>>' (TOK_SHR) for
   nested closers like Box<Box<i32>>; split it into two '>' tokens: consume one
   now and leave the other as the current token for the enclosing level. */
static int expect_gt(Parser* p) {
    if (check(p, TOK_SHR)) {
        p->current.kind = TOK_GT;
        CHECK_STRSCPY(strscpy(p->current.text, ">", sizeof(p->current.text)), "token text too long");
        return 1;
    }
    return expect(p, TOK_GT);
}

static Type parse_base_type(Parser* p) {
    Type t = {0};
    if (check(p, TOK_KW_I32)) {
        advance(p);
        t = type_make_primitive(TYPE_I32);
    } else if (check(p, TOK_KW_I8)) {
        advance(p);
        t = type_make_primitive(TYPE_I8);
    } else if (check(p, TOK_KW_I16)) {
        advance(p);
        t = type_make_primitive(TYPE_I16);
    } else if (check(p, TOK_KW_I64)) {
        advance(p);
        t = type_make_primitive(TYPE_I64);
    } else if (check(p, TOK_KW_U8)) {
        advance(p);
        t = type_make_primitive(TYPE_U8);
    } else if (check(p, TOK_KW_U16)) {
        advance(p);
        t = type_make_primitive(TYPE_U16);
    } else if (check(p, TOK_KW_U32)) {
        advance(p);
        t = type_make_primitive(TYPE_U32);
    } else if (check(p, TOK_KW_U64)) {
        advance(p);
        t = type_make_primitive(TYPE_U64);
    } else if (check(p, TOK_KW_F32)) {
        advance(p);
        t = type_make_primitive(TYPE_F32);
    } else if (check(p, TOK_KW_F64)) {
        advance(p);
        t = type_make_primitive(TYPE_F64);
    } else if (check(p, TOK_KW_BOOL)) {
        advance(p);
        t = type_make_primitive(TYPE_BOOL);
    } else if (check(p, TOK_KW_OBJECT)) {
        advance(p);
        t.type_kind = TYPE_OBJECT;
        t.is_pointer = 1;
        t.type_id = TYPE_ID_OBJECT;
    } else if (check(p, TOK_IDENT) && strcmp(p->current.text, "void") == 0) {
        advance(p);
        t.type_kind = TYPE_VOID;
    } else if (check(p, TOK_KW_STRING)) {
        advance(p);
        t = type_make_user(TYPE_CLASS, "String");
        t.is_pointer = 1;
        t.type_id = TYPE_ID_STRING;
    } else if (check(p, TOK_IDENT)) {
        const char* name = p->current.text;
        char qname[NAME_BUF_SIZE];
        /* Same-namespace-first: inside `namespace N { ... }`, an unqualified
           type name that matches a type declared in N resolves to "N_name".
           Forward decls are pre-registered, so later declarations match too. */
        if (p->ns_prefix[0] && !parser_is_type_param(name)) {
            int n = snprintf(qname, sizeof(qname), "%s_%s", p->ns_prefix, name);
            CHECK_SNPRINTF(n, sizeof(qname), "qualified type name too long");
            if (symtab_find_class(qname) || symtab_find_struct(qname) ||
                symtab_find_interface(qname) || symtab_find_enum(qname)) {
                name = qname;
            }
        }
        if (parser_is_type_param(name)) {
            Type t = type_make_param(name);
            advance(p);
            return t;
        }
        ClassInfo* ci = symtab_find_class(name);
        StructInfo* si = symtab_find_struct(name);
        InterfaceInfo* ii = symtab_find_interface(name);
        EnumInfo* ei = symtab_find_enum(name);
        if (ci) {
            t = type_make_user(TYPE_CLASS, name);
            t.is_pointer = 1;
            t.type_id = ci->type_id;
        } else if (si) {
            t = type_make_user(TYPE_STRUCT, name);
            t.type_id = si->type_id;
        } else if (ii) {
            t = type_make_user(TYPE_INTERFACE, name);
            t.type_id = ii->type_id;
        } else if (ei) {
            /* Enums are a pure compile-time type: no runtime type_id. */
            t = type_make_user(TYPE_ENUM, name);
        } else {
            fprintf(stderr, "%s(%d,%d): error: unknown type '%s'\n",
                    parser_filename(p), p->current.line, p->current.col, name);
            p->had_error = 1;
            t.type_kind = TYPE_VOID;
        }
        advance(p);
    } else {
        fprintf(stderr, "%s(%d,%d): error: expected type\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
        t.type_kind = TYPE_VOID;
    }

    if (check(p, TOK_LT)) {
        ClassInfo* ci = symtab_find_class(t.class_name);
        if (t.type_kind != TYPE_CLASS || !ci || !ci->is_generic) {
            fprintf(stderr, "%s(%d,%d): error: type '%s' does not accept type arguments\n",
                    parser_filename(p), p->current.line, p->current.col, t.class_name);
            p->had_error = 1;
        }
        advance(p); /* < */
        int count = 0;
        if (!check(p, TOK_GT)) {
            do {
                Type arg = parse_base_type(p);
                if (count < MAX_TYPE_ARGS) {
                    type_set_arg(&t, count, &arg);
                } else {
                    fprintf(stderr, "%s(%d,%d): error: too many type arguments for '%s' (max %d)\n",
                            parser_filename(p), p->current.line, p->current.col, t.class_name, MAX_TYPE_ARGS);
                    p->had_error = 1;
                }
                count++;
            } while (check(p, TOK_COMMA) && (advance(p), 1));
        }
        expect_gt(p);
        t.type_id = 0;
        type_mangled_name(&t);
    }

    return t;
}

static int type_is_primitive_value(TypeKind k) {
    return (k >= TYPE_I8 && k <= TYPE_F64) || k == TYPE_BOOL;
}

static Type parse_type(Parser* p) {
    int is_weak = 0;
    int is_unowned = 0;
    int is_const = 0;
    if (check(p, TOK_KW_CONST)) {
        is_const = 1;
        advance(p);
    }
    if (check(p, TOK_KW_WEAK)) {
        is_weak = 1;
        advance(p);
    } else if (check(p, TOK_KW_UNOWNED)) {
        is_unowned = 1;
        advance(p);
    }

    Type t = parse_base_type(p);

    if (is_weak) {
        if (t.type_kind != TYPE_CLASS && t.type_kind != TYPE_INTERFACE) {
            fprintf(stderr, "%s(%d,%d): error: weak requires a class or interface type\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
        } else {
            t.is_weak = 1;
            t.mangled_name[0] = '\0';
        }
    }

    if (is_unowned) {
        if (t.type_kind != TYPE_CLASS) {
            fprintf(stderr, "%s(%d,%d): error: unowned requires a class type\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
        } else {
            t.is_unowned = 1;
            t.mangled_name[0] = '\0';
        }
    }

    if (check(p, TOK_LBRACKET)) {
        if (is_unowned) {
            fprintf(stderr, "%s(%d,%d): error: unowned arrays are not supported\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
        }
        advance(p);
        t.is_array = 1;
        if (check(p, TOK_RBRACKET)) {
            advance(p);
            t.is_pointer = 1;
        } else {
            fprintf(stderr, "%s(%d,%d): error: fixed-size arrays are not supported; use 'T[]' dynamic arrays\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            expect(p, TOK_RBRACKET);
        }
        t.mangled_name[0] = '\0';
    }

    if (is_const) {
        if (t.is_array || t.is_weak || t.is_unowned || !type_is_primitive_value(t.type_kind)) {
            fprintf(stderr, "%s(%d,%d): error: const is only supported on primitive value types\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
        } else {
            t.is_const = 1;
        }
    }
    return t;
}


/* Parse a small expression snippet (used for f-string interpolation). */
static AstNode* parse_expr_from_text(Parser* outer, const char* text, int line, int col) {
    Lexer sub_lexer;
    lexer_init(&sub_lexer, text);
    lexer_set_filename(&sub_lexer, lexer_filename(outer->lexer));
    Parser sub;
    parser_init(&sub, &sub_lexer);
    /* Keep the enclosing namespace context so f-string interpolations inside
       a namespace block resolve names the same way as surrounding code. */
    CHECK_STRSCPY(strscpy(sub.ns_prefix, outer->ns_prefix, sizeof(sub.ns_prefix)),
                  "namespace prefix too long");
    AstNode* e = parse_expr(&sub);
    if (parser_had_error(&sub)) {
        outer->had_error = 1;
    }
    (void)line;
    (void)col;
    return e;
}

/* Append a literal text segment to an f-string part list. */
static AstNode* fstring_append_literal(AstNode* parts, Token str_tok,
                                       const char* text, int len, Type string_type) {
    Token lit = str_tok;
    lit.kind = TOK_STRING_LIT;
    if (len > 255) len = 255;
    memcpy(lit.text, text, len);
    lit.text[len] = '\0';
    AstNode* ln = ast_new_node(AST_STRING_LIT, lit);
    ln->ast_resolved_type = string_type;
    return ast_append_list(parts, ln);
}

/* Convert the escaped-brace sentinel bytes produced by the lexer (see
   token.h) back into real brace characters, in place. */
static void unescape_brace_sentinels(char* text) {
    char* s;
    for (s = text; *s; s++) {
        if (*s == TOK_ESC_LBRACE) *s = '{';
        else if (*s == TOK_ESC_RBRACE) *s = '}';
    }
}

/* Parse an f-string after the leading 'f' has been consumed.  The string token
   text already has escape sequences resolved; '\{' and '\}' arrive as sentinel
   bytes.  '{{', '}}' and the sentinel bytes produce literal braces; a single
   '{' introduces an interpolation expression. */
static AstNode* parse_fstring(Parser* p, Token str_tok) {
    AstNode* node = ast_new_node(AST_FSTRING, str_tok);
    Type string_type = type_make_user(TYPE_CLASS, "String");
    string_type.is_pointer = 1;
    string_type.type_id = TYPE_ID_STRING;
    node->ast_resolved_type = string_type;

    AstNode* parts = NULL;
    const char* s = str_tok.text;
    int n = (int)strlen(s);
    int i = 0;

    /* Literal text is accumulated in lit_buf so escape pairs can collapse
       into single braces; it is flushed into a string-literal part before
       each interpolation and once at the end. */
    char lit_buf[256];
    int lit_len = 0;

    while (i < n) {
        char c = s[i];
        if (c == '{') {
            if (i + 1 < n && s[i + 1] == '{') {
                if (lit_len < 255) lit_buf[lit_len++] = '{';
                i += 2;
                continue;
            }
            if (lit_len > 0) {
                parts = fstring_append_literal(parts, str_tok, lit_buf, lit_len,
                                               string_type);
                lit_len = 0;
            }
            i++;
            int depth = 1;
            int expr_start = i;
            while (i < n && depth > 0) {
                if (s[i] == '{') depth++;
                else if (s[i] == '}') depth--;
                i++;
            }
            if (depth != 0) {
                fprintf(stderr, "%s(%d,%d): error: unclosed expression in f-string\n",
                        parser_filename(p), str_tok.line, str_tok.col);
                p->had_error = 1;
                break;
            }
            int expr_len = i - expr_start - 1;
            if (expr_len <= 0) {
                fprintf(stderr, "%s(%d,%d): error: empty expression in f-string\n",
                        parser_filename(p), str_tok.line, str_tok.col);
                p->had_error = 1;
            } else {
                char* expr_text = (char*)calloc(1, expr_len + 1);
                if (!expr_text) {
                    fprintf(stderr, "error: out of memory parsing f-string\n");
                    p->had_error = 1;
                } else {
                    memcpy(expr_text, s + expr_start, expr_len);
                    expr_text[expr_len] = '\0';
                    AstNode* expr = parse_expr_from_text(p, expr_text, str_tok.line,
                                                         str_tok.col + expr_start);
                    free(expr_text);
                    if (expr) {
                        if (expr_contains_assign(expr) || expr_contains_inc_dec(expr)) {
                            fprintf(stderr, "%s(%d,%d): error: assignment or increment/decrement not allowed in f-string expression\n",
                                    parser_filename(p), expr->ast_token.line, expr->ast_token.col);
                            p->had_error = 1;
                        }
                        parts = ast_append_list(parts, expr);
                    }
                }
            }
        } else if (c == '}' && i + 1 < n && s[i + 1] == '}') {
            if (lit_len < 255) lit_buf[lit_len++] = '}';
            i += 2;
        } else {
            if (c == TOK_ESC_LBRACE) c = '{';
            else if (c == TOK_ESC_RBRACE) c = '}';
            if (lit_len < 255) lit_buf[lit_len++] = c;
            i++;
        }
    }

    if (!p->had_error && lit_len > 0) {
        parts = fstring_append_literal(parts, str_tok, lit_buf, lit_len,
                                       string_type);
    }

    node->ast_children[0] = parts;
    node->ast_child_count = 1;
    return node;
}

static AstNode* parse_primary(Parser* p) {
    if (check(p, TOK_INT_LIT)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_INT_LIT, t);
        n->ast_resolved_type.type_kind = TYPE_I32;
        n->ast_resolved_type.type_id = TYPE_ID_I32;
        return n;
    }
    if (check(p, TOK_FLOAT_LIT)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_FLOAT_LIT, t);
        int len = (int)strlen(t.text);
        int is_f32 = (len > 0 && (t.text[len - 1] == 'f' || t.text[len - 1] == 'F'));
        if (is_f32) {
            n->ast_resolved_type.type_kind = TYPE_F32;
            n->ast_resolved_type.type_id = TYPE_ID_F32;
        } else {
            n->ast_resolved_type.type_kind = TYPE_F64;
            n->ast_resolved_type.type_id = TYPE_ID_F64;
        }
        return n;
    }
    if (check(p, TOK_CHAR_LIT)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_CHAR_LIT, t);
        n->ast_resolved_type.type_kind = TYPE_I8;
        n->ast_resolved_type.type_id = TYPE_ID_I8;
        return n;
    }
    if (check(p, TOK_KW_TRUE) || check(p, TOK_KW_FALSE)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_BOOL_LIT, t);
        n->ast_token.int_val = (t.kind == TOK_KW_TRUE) ? 1 : 0;
        n->ast_resolved_type.type_kind = TYPE_BOOL;
        n->ast_resolved_type.type_id = TYPE_ID_BOOL;
        return n;
    }
    if (check(p, TOK_KW_NULL)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_NULL, t);
        n->ast_resolved_type.type_kind = TYPE_NULL;
        return n;
    }
    if (check(p, TOK_IDENT) && strcmp(p->current.text, "f") == 0 &&
        p->peek.kind == TOK_STRING_LIT) {
        Token ftok = p->current;
        advance(p);
        Token str_tok = p->current;
        advance(p);
        (void)ftok;
        return parse_fstring(p, str_tok);
    }
    if (check(p, TOK_STRING_LIT)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_STRING_LIT, t);
        unescape_brace_sentinels(n->ast_token.text);
        n->ast_resolved_type = type_make_user(TYPE_CLASS, "String");
        n->ast_resolved_type.is_pointer = 1;
        n->ast_resolved_type.type_id = TYPE_ID_STRING;
        return n;
    }
    if (check(p, TOK_IDENT)) {
        Token t = p->current; advance(p);
        return ast_new_node(AST_IDENT, t);
    }
    if (check(p, TOK_KW_THIS)) {
        Token t = p->current; advance(p);
        t.kind = TOK_IDENT;
        CHECK_STRSCPY(strscpy(t.text, "this", sizeof(t.text)), "token text too long");
        return ast_new_node(AST_IDENT, t);
    }
    if (check(p, TOK_LPAREN)) {
        advance(p);
        AstNode* e = parse_expr(p);
        expect(p, TOK_RPAREN);
        return e;
    }
    if (check(p, TOK_KW_NEW)) {
        Token new_tok = p->current; advance(p);

        int is_weak = 0;
        if (check(p, TOK_KW_WEAK)) {
            is_weak = 1;
            advance(p);
        }
        if (check(p, TOK_KW_UNOWNED)) {
            fprintf(stderr, "%s(%d,%d): error: 'new' cannot be used with unowned\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            advance(p);
        }

        Type base = parse_base_type(p);
        if (base.type_kind == TYPE_VOID) {
            fprintf(stderr, "%s(%d,%d): error: expected type after 'new'\n",
                    parser_filename(p), new_tok.line, new_tok.col);
            p->had_error = 1;
            return NULL;
        }

        if (is_weak) {
            if (base.type_kind != TYPE_CLASS && base.type_kind != TYPE_INTERFACE) {
                fprintf(stderr, "%s(%d,%d): error: weak requires a class or interface type\n",
                        parser_filename(p), new_tok.line, new_tok.col);
                p->had_error = 1;
            } else {
                base.is_weak = 1;
                base.mangled_name[0] = '\0';
            }
        }

        AstNode* node = ast_new_node(AST_NEW, new_tok);
        node->ast_resolved_type = base;

        if (check(p, TOK_LBRACKET)) {
            advance(p);
            if (!p->had_error) {
                fprintf(stderr, "%s(%d,%d): error: arrays are created empty; use '%s[] name; name.reserve(size)' instead of 'new %s[...]'\n",
                        parser_filename(p), new_tok.line, new_tok.col, base.class_name, base.class_name);
            }
            p->had_error = 1;
            if (!check(p, TOK_RBRACKET)) {
                parse_expr_no_assign(p, "discarded array size");
            }
            expect(p, TOK_RBRACKET);
            return node;
        }
        if (base.type_kind == TYPE_INTERFACE) {
            fprintf(stderr, "%s(%d,%d): error: cannot create instance of interface '%s'\n",
                    parser_filename(p), new_tok.line, new_tok.col, base.class_name);
            p->had_error = 1;
        } else if (base.type_kind == TYPE_OBJECT) {
            fprintf(stderr, "%s(%d,%d): error: cannot use 'new' on 'object'; instantiate a concrete class instead\n",
                    parser_filename(p), new_tok.line, new_tok.col);
            p->had_error = 1;
        } else if (base.type_kind == TYPE_STRUCT) {
            fprintf(stderr, "%s(%d,%d): error: cannot use 'new' on a struct value\n",
                    parser_filename(p), new_tok.line, new_tok.col);
            p->had_error = 1;
        }
        return node;
    }

    fprintf(stderr, "%s(%d,%d): error: unexpected token '%s' in expression\n",
            parser_filename(p), p->current.line, p->current.col, p->current.text);
    p->had_error = 1;
    return NULL;
}

static AstNode* parse_postfix(Parser* p) {
    AstNode* node = parse_primary(p);
    if (!node) return NULL;

    for (;;) {
        if (check(p, TOK_LBRACKET)) {
            Token t = p->current; advance(p);
            AstNode* idx = parse_expr_no_assign(p, "array index");
            AstNode* arr = ast_new_node(AST_ARRAY_ACCESS, t);
            ast_add_child(arr, node);
            ast_add_child(arr, idx);
            expect(p, TOK_RBRACKET);
            node = arr;
        } else if (check(p, TOK_DOT)) {
            advance(p);
            if (!check(p, TOK_IDENT)) {
                fprintf(stderr, "%s(%d,%d): error: expected field name after '.'\n",
                        parser_filename(p), p->current.line, p->current.col);
                p->had_error = 1;
                return node;
            }
            Token field = p->current; advance(p);
            AstNode* mem = ast_new_node(AST_MEMBER_ACCESS, field);
            ast_add_child(mem, node);
            node = mem;
        } else if (check(p, TOK_LPAREN)) {
            Token pt = p->current; advance(p);
            AstNode* call = ast_new_node(AST_CALL, pt);
            ast_add_child(call, node);
            if (!check(p, TOK_RPAREN)) {
                AstNode* args = NULL;
                for (;;) {
                    AstNode* arg;
                    if (check(p, TOK_KW_REF)) {
                        Token rt = p->current;
                        advance(p);
                        AstNode* inner = parse_expr_no_assign(p, "call argument");
                        arg = ast_new_node(AST_REF_ARG, rt);
                        ast_add_child(arg, inner);
                    } else {
                        arg = parse_expr_no_assign(p, "call argument");
                    }
                    args = ast_append_list(args, arg);
                    if (check(p, TOK_COMMA)) {
                        advance(p);
                        continue;
                    }
                    break;
                }
                if (args) ast_add_child(call, args);
            }
            expect(p, TOK_RPAREN);
            node = call;
        } else if (check(p, TOK_INC) || check(p, TOK_DEC)) {
            Token op = p->current; advance(p);
            if (node->ast_kind == AST_INC_DEC) {
                fprintf(stderr, "%s(%d,%d): error: invalid increment/decrement expression\n",
                        parser_filename(p), op.line, op.col);
                p->had_error = 1;
            }
            AstNode* incdec = ast_new_node(AST_INC_DEC, op);
            ast_add_child(incdec, node);
            node = incdec;
        } else {
            break;
        }
    }

    if (check(p, TOK_KW_AS)) {
        Token t = p->current; advance(p);
        Type target = parse_type(p);
        if (target.type_kind == TYPE_VOID && !p->had_error) {
            fprintf(stderr, "%s(%d,%d): error: expected type name after 'as'\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
        }
        if (target.is_unowned) {
            fprintf(stderr, "%s(%d,%d): error: unowned is not a valid cast target\n",
                    parser_filename(p), t.line, t.col);
            p->had_error = 1;
        }
        AstNode* cast = ast_new_node(AST_AS_CAST, t);
        ast_add_child(cast, node);
        cast->ast_resolved_type = target;
        node = cast;
    }

    return node;
}

static AstNode* parse_unary(Parser* p) {
    if (check(p, TOK_INC) || check(p, TOK_DEC)) {
        Token op = p->current; advance(p);
        AstNode* operand = parse_unary(p);
        if (operand && operand->ast_kind == AST_INC_DEC) {
            fprintf(stderr, "%s(%d,%d): error: invalid increment/decrement expression\n",
                    parser_filename(p), op.line, op.col);
            p->had_error = 1;
        }
        AstNode* node = ast_new_node(AST_INC_DEC, op);
        ast_add_child(node, operand);
        return node;
    }
    if (check(p, TOK_MINUS) || check(p, TOK_NOT) || check(p, TOK_TILDE)) {
        Token op = p->current; advance(p);
        AstNode* operand = parse_unary(p);
        AstNode* node = ast_new_node(AST_UNARY, op);
        ast_add_child(node, operand);
        return node;
    }
    return parse_postfix(p);
}

static AstNode* parse_binary(Parser* p,
    AstNode* (*next)(Parser*),
    TokenKind a, TokenKind b, TokenKind c) {
    AstNode* left = next(p);
    if (!left) return NULL;
    for (;;) {
        TokenKind cur = p->current.kind;
        if (cur == a || (b && cur == b) || (c && cur == c)) {
            Token op = p->current; advance(p);
            AstNode* right = next(p);
            AstNode* bin = ast_new_node(AST_BINARY, op);
            ast_add_child(bin, left);
            ast_add_child(bin, right);
            left = bin;
        } else {
            break;
        }
    }
    return left;
}

static AstNode* parse_multiplicative(Parser* p) { return parse_binary(p, parse_unary,           TOK_STAR, TOK_SLASH, TOK_PERCENT); }
static AstNode* parse_additive(Parser* p)       { return parse_binary(p, parse_multiplicative,   TOK_PLUS, TOK_MINUS, (TokenKind)0); }
static AstNode* parse_shift(Parser* p)          { return parse_binary(p, parse_additive,         TOK_SHL, TOK_SHR, (TokenKind)0); }
static AstNode* parse_relational(Parser* p)     { return parse_binary(p, parse_shift,            TOK_LT, TOK_LE, TOK_GT); }
/* TOK_GE doesn't fit in parse_binary(3 ops) ??handle inline below */

static AstNode* parse_relational_full(Parser* p) {
    AstNode* left = parse_shift(p);
    if (!left) return NULL;
    while (check(p, TOK_LT) || check(p, TOK_LE) || check(p, TOK_GT) || check(p, TOK_GE)) {
        Token op = p->current; advance(p);
        AstNode* right = parse_shift(p);
        AstNode* bin = ast_new_node(AST_BINARY, op);
        ast_add_child(bin, left);
        ast_add_child(bin, right);
        left = bin;
    }
    return left;
}

static AstNode* parse_equality(Parser* p)       { return parse_binary(p, parse_relational_full, TOK_EQ, TOK_NE, (TokenKind)0); }
static AstNode* parse_bitand(Parser* p)         { return parse_binary(p, parse_equality,        TOK_AMP, (TokenKind)0, (TokenKind)0); }
static AstNode* parse_bitxor(Parser* p)         { return parse_binary(p, parse_bitand,          TOK_CARET, (TokenKind)0, (TokenKind)0); }
static AstNode* parse_bitor(Parser* p)          { return parse_binary(p, parse_bitxor,          TOK_PIPE, (TokenKind)0, (TokenKind)0); }
static AstNode* parse_logical_and(Parser* p)    { return parse_binary(p, parse_bitor,           TOK_AND, (TokenKind)0, (TokenKind)0); }
static AstNode* parse_logical_or(Parser* p)     { return parse_binary(p, parse_logical_and,     TOK_OR, (TokenKind)0, (TokenKind)0); }

static AstNode* parse_assignment(Parser* p) {
    AstNode* left = parse_logical_or(p);
    if (!left) return NULL;
    if (check(p, TOK_ASSIGN) || check(p, TOK_PLUS_ASSIGN) || check(p, TOK_MINUS_ASSIGN) ||
        check(p, TOK_STAR_ASSIGN) || check(p, TOK_SLASH_ASSIGN) ||
        check(p, TOK_AMP_ASSIGN) || check(p, TOK_PIPE_ASSIGN) || check(p, TOK_CARET_ASSIGN) ||
        check(p, TOK_SHL_ASSIGN) || check(p, TOK_SHR_ASSIGN)) {
        Token op = p->current; advance(p);
        AstNode* right = parse_assignment(p);
        AstNode* assign = ast_new_node(AST_ASSIGN, op);
        ast_add_child(assign, left);
        ast_add_child(assign, right);
        return assign;
    }
    return left;
}

static AstNode* parse_expr(Parser* p) { return parse_assignment(p); }

static AstNode* parse_expr_no_assign(Parser* p, const char* where) {
    AstNode* e = parse_expr(p);
    if (e && (expr_contains_assign(e) || expr_contains_inc_dec(e))) {
        fprintf(stderr, "%s(%d,%d): error: assignment or increment/decrement not allowed in %s\n",
                parser_filename(p), e->ast_token.line, e->ast_token.col, where);
        p->had_error = 1;
    }
    return e;
}

/* ================================================================
   STATEMENT PARSING
   ================================================================ */

static AstNode* parse_block(Parser* p) {
    Token brace = p->current; advance(p); /* consume { */
    AstNode* block = ast_new_node(AST_BLOCK, brace);
    AstNode* stmts = NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        AstNode* s = parse_stmt(p);
        if (s) stmts = ast_append_list(stmts, s);
    }
    expect(p, TOK_RBRACE);
    ast_add_child(block, stmts);
    return block;
}

static int is_type_token(Parser* p) {
    return check(p, TOK_KW_U8)   || check(p, TOK_KW_U16)  ||
           check(p, TOK_KW_U32)  || check(p, TOK_KW_U64)  ||
           check(p, TOK_KW_I8)   || check(p, TOK_KW_I16)  ||
           check(p, TOK_KW_I32)  || check(p, TOK_KW_I64)  ||
           check(p, TOK_KW_F32)  || check(p, TOK_KW_F64)  ||
           check(p, TOK_KW_BOOL) ||
           check(p, TOK_KW_OBJECT) ||
           check(p, TOK_KW_STRING);
}

static int stmt_looks_like_var_decl(Parser* p) {
    if (check(p, TOK_KW_WEAK)) return 1;
    if (check(p, TOK_KW_UNOWNED)) return 1;
    if (check(p, TOK_KW_CONST)) return 1;
    if (is_type_token(p)) return 1;
    if (check(p, TOK_IDENT) && is_type_name(p->current.text)) {
        TokenKind next = p->peek.kind;
        if (next == TOK_IDENT) return 1;
        if (next == TOK_LBRACKET) return 1;
        if (next == TOK_LT) {
            ClassInfo* ci = symtab_find_class(p->current.text);
            if (ci && ci->is_generic) return 1;
        }
    }
    return 0;
}

static AstNode* parse_var_decl(Parser* p) {
    Type type = parse_type(p);
    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "%s(%d,%d): error: expected variable name\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
        expect(p, TOK_SEMI);
        return NULL;
    }
    Token name = p->current; advance(p);

    AstNode* node = ast_new_node(AST_VAR_DECL, name);

    if (check(p, TOK_ASSIGN)) {
        advance(p);
        AstNode* init = parse_expr(p);
        ast_add_child(node, init);
        if (init && !p->had_error && !expr_is_direct_assignment(init) && (expr_contains_assign(init) || expr_contains_inc_dec(init))) {
            fprintf(stderr, "%s(%d,%d): error: assignment or increment/decrement not allowed in variable initializer\n",
                    parser_filename(p), init->ast_token.line, init->ast_token.col);
            p->had_error = 1;
        }
        if (init && init->ast_kind == AST_NEW && type.type_kind == TYPE_CLASS) {
            type.is_pointer = 1;
            /* Target-typed 'new': `Box<i32> b = new Box;` copies the type
               arguments from the declared type onto the new expression. */
            Type* nt = &init->ast_resolved_type;
            if (nt->type_kind == TYPE_CLASS && nt->type_arg_count == 0 &&
                type.type_arg_count > 0 && strcmp(nt->class_name, type.class_name) == 0) {
                int i;
                for (i = 0; i < type.type_arg_count && i < MAX_TYPE_ARGS; i++) {
                    type_set_arg(nt, i, type.type_args[i]);
                }
                nt->mangled_name[0] = '\0';
            }
        }
    }

    node->ast_resolved_type = type;
    symtab_insert(name.text, type);
    expect(p, TOK_SEMI);
    return node;
}

/* foreach (T x in arr) { body } - array iteration, C# style.  The loop
   variable is bound only inside the body; the collection expression is
   parsed before the binding so it cannot see the loop variable. */
static AstNode* parse_foreach_stmt(Parser* p) {
    Token kw = p->current;
    advance(p);
    expect(p, TOK_LPAREN);

    Type type = parse_type(p);
    Token name = kw;
    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "%s(%d,%d): error: expected foreach variable name after the element type\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
    } else {
        name = p->current;
        advance(p);
    }
    expect(p, TOK_KW_IN);
    AstNode* arr = parse_expr(p);
    expect(p, TOK_RPAREN);

    AstNode* decl = ast_new_node(AST_VAR_DECL, name);
    decl->ast_resolved_type = type;

    symtab_enter_scope();
    if (name.kind == TOK_IDENT) {
        symtab_insert(name.text, type);
    }
    AstNode* body = parse_required_block(p, "foreach", 0);
    symtab_exit_scope();

    AstNode* node = ast_new_node(AST_FOREACH_STMT, kw);
    ast_add_child(node, decl);
    ast_add_child(node, arr);
    ast_add_child(node, body);
    return node;
}

static AstNode* parse_for_stmt(Parser* p) {
    Token kw = p->current;
    advance(p);
    expect(p, TOK_LPAREN);

    AstNode* init = NULL;
    if (!check(p, TOK_SEMI)) {
        if (stmt_looks_like_var_decl(p)) {
            init = parse_var_decl(p);
        } else {
            init = parse_expr(p);
            expect(p, TOK_SEMI);
        }
    } else {
        advance(p);
    }

    AstNode* cond = NULL;
    if (!check(p, TOK_SEMI)) {
        cond = parse_expr(p);
        if (expr_contains_assign(cond) || expr_contains_inc_dec(cond)) {
            fprintf(stderr, "%s(%d,%d): error: assignment or increment/decrement not allowed in for condition\n",
                    parser_filename(p), cond->ast_token.line, cond->ast_token.col);
            p->had_error = 1;
        }
    }
    expect(p, TOK_SEMI);

    AstNode* step = NULL;
    if (!check(p, TOK_RPAREN)) {
        step = parse_expr(p);
    }
    expect(p, TOK_RPAREN);

    AstNode* body = parse_required_block(p, "for", 0);

    AstNode* node = ast_new_node(AST_FOR_STMT, kw);
    ast_add_child(node, init);
    ast_add_child(node, cond);
    ast_add_child(node, step);
    ast_add_child(node, body);
    return node;
}

static AstNode* parse_match_stmt(Parser* p) {
    Token kw = p->current;
    advance(p); /* match */
    expect(p, TOK_LPAREN);

    AstNode* expr = parse_expr(p);
    if (expr && (expr_contains_assign(expr) || expr_contains_inc_dec(expr))) {
        fprintf(stderr, "%s(%d,%d): error: assignment or increment/decrement not allowed in match expression\n",
                parser_filename(p), expr->ast_token.line, expr->ast_token.col);
        p->had_error = 1;
    }
    expect(p, TOK_RPAREN);
    expect(p, TOK_LBRACE);

    AstNode* arms = NULL;
    int saw_else = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        AstNode* arm = NULL;
        int arm_scope_entered = 0;
        if (saw_else) {
            fprintf(stderr, "%s(%d,%d): error: arm after 'else' is not allowed\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
        }
        if (check(p, TOK_KW_ELSE)) {
            Token else_tok = p->current;
            advance(p);
            arm = ast_new_node(AST_MATCH_ARM, else_tok);
            arm->ast_resolved_type.type_kind = TYPE_VOID;
            saw_else = 1;
        } else if (check(p, TOK_INT_LIT)) {
            Token lit = p->current;
            advance(p);
            arm = ast_new_node(AST_MATCH_ARM, lit);
            arm->ast_resolved_type = type_make_primitive(TYPE_I32);
        } else if (check(p, TOK_IDENT) && p->peek.kind == TOK_DOT &&
                   symtab_find_enum(p->current.text)) {
            /* Enum variant constant arm: EnumName.Variant.  Variant existence
               is validated in codegen: the enum body may be parsed after the
               function that contains this match (names are pre-registered). */
            Token enum_tok = p->current;
            advance(p); /* enum name */
            advance(p); /* . */
            if (!check(p, TOK_IDENT)) {
                fprintf(stderr, "%s(%d,%d): error: expected variant name after '%s.'\n",
                        parser_filename(p), p->current.line, p->current.col, enum_tok.text);
                p->had_error = 1;
            }
            Token variant = p->current;
            if (check(p, TOK_IDENT)) advance(p);
            arm = ast_new_node(AST_MATCH_ARM, variant);
            arm->ast_resolved_type = type_make_user(TYPE_ENUM, enum_tok.text);
        } else if (check(p, TOK_IDENT)) {
            /* Type pattern: ClassName var */
            Type pattern_type = parse_base_type(p);
            if (pattern_type.type_kind != TYPE_CLASS) {
                fprintf(stderr, "%s(%d,%d): error: match type pattern must be a class name\n",
                        parser_filename(p), p->current.line, p->current.col);
                p->had_error = 1;
            }
            if (!check(p, TOK_IDENT)) {
                fprintf(stderr, "%s(%d,%d): error: expected variable name after type pattern\n",
                        parser_filename(p), p->current.line, p->current.col);
                p->had_error = 1;
                if (check(p, TOK_FATARROW)) { advance(p); }
                parse_stmt(p);
                continue;
            }
            Token var = p->current;
            advance(p);
            arm = ast_new_node(AST_MATCH_ARM, var);
            arm->ast_resolved_type = pattern_type;
            CHECK_STRSCPY(strscpy(arm->ast_match_var, pattern_type.class_name, sizeof(arm->ast_match_var)),
                          "match type name too long");
            /* Bind the variable for the arm body. */
            symtab_enter_scope();
            arm_scope_entered = 1;
            symtab_insert(var.text, pattern_type);
        } else {
            fprintf(stderr, "%s(%d,%d): error: expected 'else', integer literal, enum variant, or type pattern in match arm\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            if (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) { advance(p); }
            continue;
        }
        expect(p, TOK_FATARROW);
        AstNode* body = parse_stmt(p);
        ast_add_child(arm, body);
        arms = ast_append_list(arms, arm);
        if (arm_scope_entered) {
            /* Close the arm scope that was opened for the type pattern. */
            symtab_exit_scope();
        }
    }
    expect(p, TOK_RBRACE);

    AstNode* node = ast_new_node(AST_MATCH, kw);
    ast_add_child(node, expr);
    ast_add_child(node, arms);
    return node;
}

/* if/else/while/for bodies must be blocks.  An 'else' body may also be
   another if statement to allow else-if chains. */
static AstNode* parse_required_block(Parser* p, const char* what, int allow_if) {
    if (allow_if && check(p, TOK_KW_IF)) {
        return parse_stmt(p);
    }
    if (!check(p, TOK_LBRACE)) {
        fprintf(stderr, "%s(%d,%d): error: expected '{' for %s body\n",
                parser_filename(p), p->current.line, p->current.col, what);
        p->had_error = 1;
    }
    return parse_stmt(p);
}

static AstNode* parse_stmt(Parser* p) {
    if (check(p, TOK_LBRACE)) {
        return parse_block(p);
    }

    if (check(p, TOK_KW_FOR)) {
        return parse_for_stmt(p);
    }

    if (check(p, TOK_KW_FOREACH)) {
        return parse_foreach_stmt(p);
    }

    if (check(p, TOK_KW_IF)) {
        Token kw = p->current; advance(p);
        expect(p, TOK_LPAREN);
        AstNode* cond = parse_expr(p);
        if (expr_contains_assign(cond) || expr_contains_inc_dec(cond)) {
            fprintf(stderr, "%s(%d,%d): error: assignment or increment/decrement not allowed in if condition\n",
                    parser_filename(p), cond->ast_token.line, cond->ast_token.col);
            p->had_error = 1;
        }
        expect(p, TOK_RPAREN);
        AstNode* then_body = parse_required_block(p, "if", 0);
        AstNode* else_body = NULL;
        if (check(p, TOK_KW_ELSE)) {
            advance(p);
            else_body = parse_required_block(p, "else", 1);
        }
        AstNode* node = ast_new_node(AST_IF_STMT, kw);
        ast_add_child(node, cond);
        ast_add_child(node, then_body);
        if (else_body) ast_add_child(node, else_body);
        return node;
    }

    if (check(p, TOK_KW_WHILE)) {
        Token kw = p->current; advance(p);
        expect(p, TOK_LPAREN);
        AstNode* cond = parse_expr(p);
        if (expr_contains_assign(cond) || expr_contains_inc_dec(cond)) {
            fprintf(stderr, "%s(%d,%d): error: assignment or increment/decrement not allowed in while condition\n",
                    parser_filename(p), cond->ast_token.line, cond->ast_token.col);
            p->had_error = 1;
        }
        expect(p, TOK_RPAREN);
        AstNode* body = parse_required_block(p, "while", 0);
        AstNode* node = ast_new_node(AST_WHILE_STMT, kw);
        ast_add_child(node, cond);
        ast_add_child(node, body);
        return node;
    }

    if (check(p, TOK_KW_RETURN)) {
        Token kw = p->current; advance(p);
        AstNode* node = ast_new_node(AST_RETURN_STMT, kw);
        if (!check(p, TOK_SEMI)) {
            ast_add_child(node, parse_expr_no_assign(p, "return expression"));
        }
        expect(p, TOK_SEMI);
        return node;
    }

    if (check(p, TOK_KW_BREAK)) {
        Token kw = p->current; advance(p);
        expect(p, TOK_SEMI);
        return ast_new_node(AST_BREAK, kw);
    }

    if (check(p, TOK_KW_CONTINUE)) {
        Token kw = p->current; advance(p);
        expect(p, TOK_SEMI);
        return ast_new_node(AST_CONTINUE, kw);
    }

    if (check(p, TOK_KW_MATCH)) {
        return parse_match_stmt(p);
    }

    if (stmt_looks_like_var_decl(p)) {
        return parse_var_decl(p);
    }

    /* expression statement */
    if (!check(p, TOK_EOF) && !check(p, TOK_RBRACE)) {
        AstNode* expr = parse_expr(p);
        if (expr && !p->had_error) {
            int has_assign = expr_contains_assign(expr);
            int has_inc_dec = expr_contains_inc_dec(expr);
            int is_direct_assign = expr_is_direct_assignment(expr);
            int is_direct_inc_dec = expr_is_inc_dec(expr);
            if ((!is_direct_assign && has_assign) || (!is_direct_inc_dec && has_inc_dec)) {
                fprintf(stderr, "%s(%d,%d): error: assignment or increment/decrement not allowed in expression statement\n",
                        parser_filename(p), expr->ast_token.line, expr->ast_token.col);
                p->had_error = 1;
            }
        }
        if (expr) {
            AstNode* es = ast_new_node(AST_EXPR_STMT, expr->ast_token);
            ast_add_child(es, expr);
            expect(p, TOK_SEMI);
            return es;
        }
        /* Error recovery: skip the offending token so parsing can make progress. */
        if (!check(p, TOK_EOF) && !check(p, TOK_RBRACE) && !check(p, TOK_SEMI)) {
            advance(p);
        }
        expect(p, TOK_SEMI);
    }

    return NULL;
}


/* Parses and validates a default parameter value; the current token must be
   the '=' after the parameter name.  Defaults are literals only (integer,
   float, char, string, true/false, null) so they can be re-emitted at any
   call site without scope or side-effect concerns.  Returns the literal
   node, or NULL after reporting an error. */
static AstNode* parse_param_default(Parser* p, const Type* pt, const char* param_name, int is_ref) {
    Token eq = p->current;
    advance(p); /* consume = */
    if (is_ref) {
        fprintf(stderr, "%s(%d,%d): error: ref parameters cannot have default values\n",
                parser_filename(p), eq.line, eq.col);
        p->had_error = 1;
    }
    AstNode* lit = parse_primary(p);
    if (!lit) return NULL;
    int is_literal = lit->ast_kind == AST_INT_LIT || lit->ast_kind == AST_FLOAT_LIT ||
                     lit->ast_kind == AST_CHAR_LIT || lit->ast_kind == AST_STRING_LIT ||
                     lit->ast_kind == AST_BOOL_LIT || lit->ast_kind == AST_NULL;
    if (!is_literal) {
        fprintf(stderr, "%s(%d,%d): error: default parameter value must be a literal\n",
                parser_filename(p), lit->ast_token.line, lit->ast_token.col);
        p->had_error = 1;
        return NULL;
    }
    int ok = 1;
    switch (lit->ast_kind) {
        case AST_BOOL_LIT:
            /* bool and numeric types do not implicitly convert. */
            ok = !pt->is_array && pt->type_kind == TYPE_BOOL;
            break;
        case AST_INT_LIT:
        case AST_FLOAT_LIT:
        case AST_CHAR_LIT:
            /* Numeric literals default to numeric parameters only. */
            ok = !pt->is_array && pt->type_kind >= TYPE_I8 && pt->type_kind <= TYPE_F64;
            break;
        case AST_STRING_LIT:
            ok = !pt->is_array && pt->type_kind == TYPE_CLASS &&
                 strcmp(pt->class_name, "String") == 0;
            break;
        case AST_NULL:
            /* Same categories as a null argument at the call site: class,
               interface, and object references (weak included); unowned
               references and value types cannot be null. */
            ok = !pt->is_array && !pt->is_unowned &&
                 (pt->type_kind == TYPE_CLASS || pt->type_kind == TYPE_INTERFACE ||
                  pt->type_kind == TYPE_OBJECT);
            break;
        default:
            break;
    }
    if (!ok) {
        fprintf(stderr, "%s(%d,%d): error: default value for parameter '%s' does not match the parameter type\n",
                parser_filename(p), lit->ast_token.line, lit->ast_token.col, param_name);
        p->had_error = 1;
        return NULL;
    }
    return lit;
}

static AstNode* parse_struct_decl(Parser* p) {
    advance(p); /* struct */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "%s(%d,%d): error: expected struct name\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token name = p->current; advance(p);
    parser_qualify_decl_name(p, &name);
    expect(p, TOK_LBRACE);

    StructInfo* info = symtab_find_struct(name.text);
    if (info) {
        if (info->field_count > 0) {
            fprintf(stderr, "%s(%d,%d): error: struct '%s' already defined\n",
                    parser_filename(p), name.line, name.col, name.text);
            p->had_error = 1;
        }
    } else {
        info = (StructInfo*)calloc(1, sizeof(StructInfo));
        CHECK_STRSCPY(strscpy(info->name, name.text, sizeof(info->name)), "struct name too long");
        info->name[63] = '\0';
        info->type_id = symtab_next_type_id() | TYPE_IS_STRUCT;

        /* register early so later types can refer to it */
        symtab_add_struct(name.text, info);
    }

    AstNode* methods = NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (check(p, TOK_KW_PUBLIC) || check(p, TOK_KW_PRIVATE)) {
            fprintf(stderr, "%s(%d,%d): error: access modifiers are not allowed in structs\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            advance(p);
            continue;
        }
        if (check(p, TOK_KW_OVERRIDE)) {
            fprintf(stderr, "%s(%d,%d): error: override is not allowed in structs\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            advance(p);
            continue;
        }
        int is_native = 0;
        if (check(p, TOK_KW_NATIVE)) {
            is_native = 1;
            advance(p);
        }
        Type ft = parse_type(p);
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "%s(%d,%d): error: expected field or method name\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            break;
        }
        Token fname = p->current; advance(p);

        if (check(p, TOK_LPAREN)) {
            /* METHOD: emitted as StructName_method(StructName* thiz, ...). */
            advance(p); /* consume ( */
            if (is_native) {
                fprintf(stderr, "%s(%d,%d): error: native methods are not supported in structs\n",
                        parser_filename(p), fname.line, fname.col);
                p->had_error = 1;
            }
            if (ft.is_array) {
                fprintf(stderr, "%s(%d,%d): error: method '%s' cannot return array by value\n",
                        parser_filename(p), fname.line, fname.col, fname.text);
                p->had_error = 1;
            }

            symtab_enter_scope();

            /* 'this' aliases the receiver struct, like a ref parameter. */
            Type this_type = type_make_user(TYPE_STRUCT, name.text);
            this_type.is_ref = 1;
            symtab_insert("this", this_type);

            AstNode* mparams = NULL;
            int mc = 0;
            char mpn[MAX_PARAMS][NAME_BUF_SIZE];
            Type mpt[MAX_PARAMS];
            AstNode* mpd[MAX_PARAMS];
            int seen_default = 0;

            if (!check(p, TOK_RPAREN)) {
                do {
                    int is_ref = 0;
                    if (check(p, TOK_KW_REF)) {
                        is_ref = 1; advance(p);
                    }
                    Type pt = parse_type(p);
                    pt.is_ref = is_ref;
                    if (is_ref && pt.is_const) {
                        fprintf(stderr, "%s(%d,%d): error: ref parameters cannot be const\n",
                                parser_filename(p), p->current.line, p->current.col);
                        p->had_error = 1;
                    }
                    if (!check(p, TOK_IDENT)) {
                        fprintf(stderr, "%s(%d,%d): error: expected parameter name\n",
                                parser_filename(p), p->current.line, p->current.col);
                        p->had_error = 1;
                        break;
                    }
                    Token pn = p->current; advance(p);
                    AstNode* pdefault = NULL;
                    if (check(p, TOK_ASSIGN)) {
                        pdefault = parse_param_default(p, &pt, pn.text, is_ref);
                        seen_default = 1;
                    } else if (seen_default) {
                        fprintf(stderr, "%s(%d,%d): error: parameter '%s' must have a default value because a previous parameter has one\n",
                                parser_filename(p), pn.line, pn.col, pn.text);
                        p->had_error = 1;
                    }
                    AstNode* pd = ast_new_node(AST_VAR_DECL, pn);
                    pd->ast_resolved_type = pt;
                    symtab_insert(pn.text, pt);
                    mparams = ast_append_list(mparams, pd);
                    if (mc < MAX_PARAMS) {
                        CHECK_STRSCPY(strscpy(mpn[mc], pn.text, sizeof(mpn[mc])), "parameter name too long");
                        mpt[mc] = pt;
                        mpd[mc] = pdefault;
                        mc++;
                    }
                } while (check(p, TOK_COMMA) && (advance(p), 1));
            }
            expect(p, TOK_RPAREN);

            {
                int pi;
                for (pi = 0; pi < mc; pi++) {
                    if (mpt[pi].is_array && !mpt[pi].is_ref) {
                        fprintf(stderr, "%s(%d,%d): error: array parameter '%s' of method '%s' must be ref\n",
                                parser_filename(p), fname.line, fname.col, mpn[pi], fname.text);
                        p->had_error = 1;
                    }
                }
            }

            AstNode* mbody = parse_stmt(p);
            symtab_exit_scope();

            symtab_add_struct_method(info, fname.text, ft, mc, mpn, mpt, mpd);

            AstNode* mnode = ast_new_node(AST_FUNC_DECL, fname);
            mnode->ast_resolved_type = ft;
            if (mparams) { ast_add_child(mnode, mparams); }
            ast_add_child(mnode, mbody);
            methods = ast_append_list(methods, mnode);
            continue;
        }

        /* FIELD */
        if (ft.type_kind == TYPE_VOID || ft.is_array || ft.array_size > 0) {
            fprintf(stderr, "%s(%d,%d): error: struct fields cannot be void or array types\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            /* try to recover */
            expect(p, TOK_SEMI);
            continue;
        }
        if (ft.is_const) {
            fprintf(stderr, "%s(%d,%d): error: const fields are not supported\n",
                    parser_filename(p), fname.line, fname.col);
            p->had_error = 1;
        }
        if (symtab_add_struct_field(info, fname.text, ft) != 0) {
            fprintf(stderr, "%s(%d,%d): error: too many fields in struct '%s' (max %d)\n",
                    parser_filename(p), fname.line, fname.col, name.text, MAX_FIELDS);
            p->had_error = 1;
        }
        expect(p, TOK_SEMI);
    }
    expect(p, TOK_RBRACE);

    AstNode* node = ast_new_node(AST_STRUCT_DECL, name);
    node->ast_resolved_type.type_kind = TYPE_STRUCT;
    CHECK_STRSCPY(strscpy(node->ast_resolved_type.class_name, name.text, sizeof(node->ast_resolved_type.class_name)), "struct name too long");
    node->ast_resolved_type.type_id = info->type_id;
    if (methods) ast_add_child(node, methods);
    return node;
}

static AstNode* parse_class_decl(Parser* p) {
    advance(p); /* class */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "%s(%d,%d): error: expected class name\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token name = p->current; advance(p);
    parser_qualify_decl_name(p, &name);

    if (strcmp(name.text, "String") == 0) {
        fprintf(stderr, "%s(%d,%d): error: class name '%s' is reserved for a builtin type\n",
                parser_filename(p), name.line, name.col, name.text);
        p->had_error = 1;
    }

    ClassInfo* info = symtab_find_class(name.text);
    if (info) {
        if (info->field_count > 0 || info->methods) {
            fprintf(stderr, "%s(%d,%d): error: class '%s' already defined\n",
                    parser_filename(p), name.line, name.col, name.text);
            p->had_error = 1;
        }
    } else {
        info = (ClassInfo*)calloc(1, sizeof(ClassInfo));
        CHECK_STRSCPY(strscpy(info->name, name.text, sizeof(info->name)), "class name too long");
        info->name[63] = '\0';

        /* register early so methods with this return type resolve */
        symtab_add_class(name.text, info);
    }

    /* parse optional generic parameter list */
    if (check(p, TOK_LT)) {
        if (p->ns_prefix[0]) {
            fprintf(stderr, "%s(%d,%d): error: generic classes are not supported inside namespaces\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
        }
        advance(p); /* < */
        char params[MAX_GENERIC_PARAMS][64];
        int param_count = 0;
        do {
            if (param_count >= MAX_GENERIC_PARAMS) {
                fprintf(stderr, "%s(%d,%d): error: too many generic parameters for class '%s' (max %d)\n",
                        parser_filename(p), p->current.line, p->current.col, name.text, MAX_GENERIC_PARAMS);
                p->had_error = 1;
                break;
            }
            if (!check(p, TOK_IDENT)) {
                fprintf(stderr, "%s(%d,%d): error: expected type parameter name\n",
                        parser_filename(p), p->current.line, p->current.col);
                p->had_error = 1;
                break;
            }
            CHECK_STRSCPY(strscpy(params[param_count], p->current.text, sizeof(params[param_count])),
                          "generic parameter name too long");
            param_count++;
            advance(p);

            int param_idx = param_count - 1;
            if (check(p, TOK_COLON)) {
                advance(p); /* : */
                do {
                    if (check(p, TOK_KW_NEW)) {
                        advance(p);
                        expect(p, TOK_LPAREN);
                        expect(p, TOK_RPAREN);
                        info->generic_has_new[param_idx] = 1;
                    } else if (check(p, TOK_IDENT)) {
                        Token c = p->current; advance(p);
                        if (!symtab_find_interface(c.text)) {
                            fprintf(stderr, "%s(%d,%d): error: unknown interface constraint '%s'\n",
                                    parser_filename(p), c.line, c.col, c.text);
                            p->had_error = 1;
                        }
                        int cc = info->generic_constraint_count[param_idx];
                        if (cc < MAX_CONSTRAINTS_PER_PARAM) {
                            CHECK_STRSCPY(strscpy(info->generic_constraints[param_idx][cc], c.text,
                                                  sizeof(info->generic_constraints[param_idx][cc])),
                                          "constraint name too long");
                            info->generic_constraint_count[param_idx]++;
                        }
                    } else {
                        fprintf(stderr, "%s(%d,%d): error: expected 'new()' or interface name in constraint\n",
                                parser_filename(p), p->current.line, p->current.col);
                        p->had_error = 1;
                        break;
                    }
                } while (check(p, TOK_COMMA) && (advance(p), 1));
            }
        } while (check(p, TOK_COMMA) && (advance(p), 1));
        expect(p, TOK_GT);
        {
            const char* param_ptrs[MAX_GENERIC_PARAMS];
            int k;
            for (k = 0; k < param_count && k < MAX_GENERIC_PARAMS; k++) {
                param_ptrs[k] = params[k];
            }
            symtab_mark_class_generic(info, NULL, param_count, param_ptrs);
        }
    }

    /* parse optional interface implementation list */
    if (check(p, TOK_COLON)) {
        advance(p);
        do {
            if (!check(p, TOK_IDENT)) {
                fprintf(stderr, "%s(%d,%d): error: expected interface name after ':'\n",
                        parser_filename(p), p->current.line, p->current.col);
                p->had_error = 1;
                break;
            }
            Token iface_tok = p->current; advance(p);
            symtab_add_class_impl(info, iface_tok.text);
        } while (check(p, TOK_COMMA) && (advance(p), 1));
    }

    /* generic definitions do not get a concrete type_id until instantiated */
    if (!info->is_generic && info->type_id == 0) {
        info->type_id = symtab_next_type_id();
    }

    expect(p, TOK_LBRACE);

    parser_push_type_params(info);

    AstNode* methods = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        int is_native = 0;
        int is_override = 0;
        int is_private = 0;
        int is_public = 0;
        int is_static = 0;
        while (check(p, TOK_KW_NATIVE) || check(p, TOK_KW_OVERRIDE) ||
               check(p, TOK_KW_PUBLIC) || check(p, TOK_KW_PRIVATE) ||
               check(p, TOK_KW_STATIC)) {
            if (check(p, TOK_KW_NATIVE)) {
                is_native = 1; advance(p);
            } else if (check(p, TOK_KW_STATIC)) {
                is_static = 1; advance(p);
            } else if (check(p, TOK_KW_OVERRIDE)) {
                is_override = 1; advance(p);
            } else {
                int want_private = check(p, TOK_KW_PRIVATE);
                if (is_public || is_private) {
                    fprintf(stderr, "%s(%d,%d): error: duplicate or conflicting access modifier\n",
                            parser_filename(p), p->current.line, p->current.col);
                    p->had_error = 1;
                }
                if (want_private) is_private = 1; else is_public = 1;
                advance(p);
            }
        }
        /* 'static const string': parse_type rejects const on class types, so
           consume const here and flag the type manually. */
        int static_const_string = 0;
        if (is_static && check(p, TOK_KW_CONST) && p->peek.kind == TOK_KW_STRING) {
            static_const_string = 1;
            advance(p); /* const */
        }
        Type ft = parse_type(p);
        if (static_const_string) ft.is_const = 1;
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "%s(%d,%d): error: expected field or method name\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            break;
        }
        Token fname = p->current; advance(p);

        if (check(p, TOK_LBRACE)) {
            /* PROPERTY: Type name { get { ... } set { ... } } (C# style).
               Accessors are synthesized as ordinary get_X/set_X methods and
               flow through the normal method machinery.  Value types and
               strong class references (including string) are allowed; weak,
               unowned, array, object, and interface types are not. */
            advance(p); /* { */

            if (is_static || is_native || is_override) {
                fprintf(stderr, "%s(%d,%d): error: static/native/override are not allowed on property '%s'\n",
                        parser_filename(p), fname.line, fname.col, fname.text);
                p->had_error = 1;
            }
            if (info->is_generic) {
                fprintf(stderr, "%s(%d,%d): error: properties are not supported in generic classes\n",
                        parser_filename(p), fname.line, fname.col);
                p->had_error = 1;
            }
            if (!type_is_primitive_value(ft.type_kind) && ft.type_kind != TYPE_ENUM &&
                !(ft.type_kind == TYPE_CLASS && !ft.is_weak && !ft.is_unowned && !ft.is_array)) {
                fprintf(stderr, "%s(%d,%d): error: property '%s' must have a primitive, bool, enum, or strong class type\n",
                        parser_filename(p), fname.line, fname.col, fname.text);
                p->had_error = 1;
            }
            /* Name collisions: fields, methods, static consts, other
               properties, and the synthesized accessor names. */
            {
                int clash = symtab_find_property(info, fname.text) != NULL ||
                            symtab_find_class_const(info->name, fname.text) != NULL ||
                            symtab_find_method_in_class(info, fname.text) != NULL;
                int fi;
                for (fi = 0; fi < info->field_count && fi < MAX_FIELDS; fi++) {
                    if (strcmp(info->field_names[fi], fname.text) == 0) { clash = 1; break; }
                }
                char acc_name[NAME_BUF_SIZE + 4];
                snprintf(acc_name, sizeof(acc_name), "get_%s", fname.text);
                if (symtab_find_method_in_class(info, acc_name)) clash = 1;
                snprintf(acc_name, sizeof(acc_name), "set_%s", fname.text);
                if (symtab_find_method_in_class(info, acc_name)) clash = 1;
                if (clash) {
                    fprintf(stderr, "%s(%d,%d): error: property '%s' conflicts with an existing member in class '%s'\n",
                            parser_filename(p), fname.line, fname.col, fname.text, name.text);
                    p->had_error = 1;
                }
            }

            PropertyInfo* pi = (PropertyInfo*)calloc(1, sizeof(PropertyInfo));
            CHECK_STRSCPY(strscpy(pi->name, fname.text, sizeof(pi->name)), "property name too long");
            pi->prop_type = ft;
            pi->is_private = is_private;

            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                if (!check(p, TOK_IDENT) ||
                    (strcmp(p->current.text, "get") != 0 && strcmp(p->current.text, "set") != 0)) {
                    fprintf(stderr, "%s(%d,%d): error: expected 'get' or 'set' accessor in property '%s'\n",
                            parser_filename(p), p->current.line, p->current.col, fname.text);
                    p->had_error = 1;
                    advance(p);
                    continue;
                }
                Token acc = p->current; advance(p);
                int is_get = strcmp(acc.text, "get") == 0;
                if ((is_get && pi->has_get) || (!is_get && pi->has_set)) {
                    fprintf(stderr, "%s(%d,%d): error: duplicate '%s' accessor in property '%s'\n",
                            parser_filename(p), acc.line, acc.col, acc.text, fname.text);
                    p->had_error = 1;
                }
                if (is_get) pi->has_get = 1; else pi->has_set = 1;

                /* Parse the accessor body as a method: 'this' is the
                   receiver; the setter gets an implicit 'value' parameter. */
                char mname[NAME_BUF_SIZE + 4];
                snprintf(mname, sizeof(mname), "%s_%s", acc.text, fname.text);
                Token mtok = acc;
                CHECK_STRSCPY(strscpy(mtok.text, mname, sizeof(mtok.text)), "property name too long");

                symtab_enter_scope();
                Type this_type = type_make_user(TYPE_CLASS, name.text);
                this_type.is_pointer = 1;
                symtab_insert("this", this_type);

                AstNode* mparams = NULL;
                int mc = 0;
                char mpn[1][64];
                Type mpt[1];
                Type ret_type = ft;
                if (!is_get) {
                    ret_type = type_make_primitive(TYPE_VOID);
                    symtab_insert("value", ft);
                    AstNode* pd = ast_new_node(AST_VAR_DECL, acc);
                    CHECK_STRSCPY(strscpy(pd->ast_token.text, "value", sizeof(pd->ast_token.text)), "param name too long");
                    pd->ast_resolved_type = ft;
                    mparams = ast_append_list(mparams, pd);
                    CHECK_STRSCPY(strscpy(mpn[0], "value", sizeof(mpn[0])), "param name too long");
                    mpt[0] = ft;
                    mc = 1;
                }

                AstNode* mbody = parse_stmt(p);
                symtab_exit_scope();

                symtab_add_method(info, mname, ret_type, mc, mpn, mpt, NULL,
                                  0, 0, is_private, 0, acc);

                AstNode* mnode = ast_new_node(AST_FUNC_DECL, mtok);
                mnode->ast_resolved_type = ret_type;
                if (mparams) { ast_add_child(mnode, mparams); }
                ast_add_child(mnode, mbody);
                methods = ast_append_list(methods, mnode);
            }
            expect(p, TOK_RBRACE);

            if (!pi->has_get && !pi->has_set) {
                fprintf(stderr, "%s(%d,%d): error: property '%s' must have at least one accessor\n",
                        parser_filename(p), fname.line, fname.col, fname.text);
                p->had_error = 1;
            }
            symtab_add_property(info, pi);
            continue;
        }

        if (check(p, TOK_LPAREN)) {
            /* METHOD */
            advance(p); /* consume ( */

            if (ft.is_unowned) {
                fprintf(stderr, "%s(%d,%d): error: unowned return type is not supported\n",
                        parser_filename(p), fname.line, fname.col);
                p->had_error = 1;
            }

            if (symtab_find_class_const(info->name, fname.text)) {
                fprintf(stderr, "%s(%d,%d): error: method '%s' conflicts with a static const in class '%s'\n",
                        parser_filename(p), fname.line, fname.col, fname.text, name.text);
                p->had_error = 1;
            }
            if (symtab_find_property(info, fname.text)) {
                fprintf(stderr, "%s(%d,%d): error: method '%s' conflicts with a property in class '%s'\n",
                        parser_filename(p), fname.line, fname.col, fname.text, name.text);
                p->had_error = 1;
            }

            if (is_static && is_native) {
                fprintf(stderr, "%s(%d,%d): error: method '%s' cannot be both static and native\n",
                        parser_filename(p), fname.line, fname.col, fname.text);
                p->had_error = 1;
            }
            if (is_static && is_override) {
                fprintf(stderr, "%s(%d,%d): error: method '%s' cannot be both static and override\n",
                        parser_filename(p), fname.line, fname.col, fname.text);
                p->had_error = 1;
            }

            symtab_enter_scope();

            /* register implicit this (static methods have no receiver) */
            if (!is_static) {
                Type this_type = type_make_user(TYPE_CLASS, name.text);
                this_type.is_pointer = 1;
                if (info->is_generic) {
                    int k;
                    for (k = 0; k < info->generic_param_count && k < MAX_TYPE_ARGS; k++) {
                        Type param = type_make_param(info->generic_params[k]);
                        type_set_arg(&this_type, k, &param);
                    }
                }
                symtab_insert("this", this_type);
            }

            AstNode* mparams = NULL;
            int mc = 0;
            char mpn[MAX_PARAMS][NAME_BUF_SIZE];
            Type mpt[MAX_PARAMS];
            AstNode* mpd[MAX_PARAMS];
            int seen_default = 0;

            if (!check(p, TOK_RPAREN)) {
                do {
                    int is_ref = 0;
                    if (check(p, TOK_KW_REF)) {
                        is_ref = 1; advance(p);
                    }
                    Type pt = parse_type(p);
                    pt.is_ref = is_ref;
                    if (is_ref && pt.is_const) {
                        fprintf(stderr, "%s(%d,%d): error: ref parameters cannot be const\n",
                                parser_filename(p), p->current.line, p->current.col);
                        p->had_error = 1;
                    }
                    if (!check(p, TOK_IDENT)) {
                        fprintf(stderr, "%s(%d,%d): error: expected parameter name\n",
                                parser_filename(p), p->current.line, p->current.col);
                        p->had_error = 1;
                        break;
                    }
                    Token pn = p->current; advance(p);
                    AstNode* pdefault = NULL;
                    if (check(p, TOK_ASSIGN)) {
                        pdefault = parse_param_default(p, &pt, pn.text, is_ref);
                        seen_default = 1;
                    } else if (seen_default) {
                        fprintf(stderr, "%s(%d,%d): error: parameter '%s' must have a default value because a previous parameter has one\n",
                                parser_filename(p), pn.line, pn.col, pn.text);
                        p->had_error = 1;
                    }
                    AstNode* pd = ast_new_node(AST_VAR_DECL, pn);
                    pd->ast_resolved_type = pt;
                    symtab_insert(pn.text, pt);
                    mparams = ast_append_list(mparams, pd);
                    if (mc < MAX_PARAMS) {
                        CHECK_STRSCPY(strscpy(mpn[mc], pn.text, sizeof(mpn[mc])), "parameter name too long");
                        mpt[mc] = pt;
                        mpd[mc] = pdefault;
                        mc++;
                    }
                } while (check(p, TOK_COMMA) && (advance(p), 1));
            }
            expect(p, TOK_RPAREN);

            if (ft.is_array) {
                fprintf(stderr, "%s(%d,%d): error: method '%s' cannot return array by value\n",
                        parser_filename(p), fname.line, fname.col, fname.text);
                p->had_error = 1;
            }
            {
                int pi;
                for (pi = 0; pi < mc; pi++) {
                    if (mpt[pi].is_array && !mpt[pi].is_ref) {
                        fprintf(stderr, "%s(%d,%d): error: array parameter '%s' of method '%s' must be ref\n",
                                parser_filename(p), fname.line, fname.col, mpn[pi], fname.text);
                        p->had_error = 1;
                    }
                }
            }

            AstNode* mbody = NULL;
            if (is_native) {
                expect(p, TOK_SEMI);
                mbody = ast_new_node(AST_BLOCK, fname);
            } else {
                mbody = parse_stmt(p);
            }

            symtab_exit_scope();

            if (is_private && is_override) {
                fprintf(stderr, "%s(%d,%d): error: method '%s' cannot be both private and override\n",
                        parser_filename(p), fname.line, fname.col, fname.text);
                p->had_error = 1;
            }

            symtab_add_method(info, fname.text, ft, mc, mpn, mpt, mpd, is_native, is_override, is_private, is_static, fname);

            AstNode* mnode = ast_new_node(AST_FUNC_DECL, fname);
            mnode->ast_resolved_type = ft;
            mnode->ast_is_native = is_native;
            mnode->ast_is_static = is_static;
            if (mparams) { ast_add_child(mnode, mparams); }
            ast_add_child(mnode, mbody);
            methods = ast_append_list(methods, mnode);

        } else {
            /* FIELD or STATIC CONST */
            if (is_static && ft.is_const) {
                /* static const member: lives in the global ConstInfo registry
                   with the class as owner; codegen emits Class_NAME and
                   member access resolves Class.MAX.  parse_type has already
                   restricted const to primitive value types (string comes in
                   via static_const_string), so no further type check here. */
                if (is_native || is_override) {
                    fprintf(stderr, "%s(%d,%d): error: native/override are not allowed on static const '%s'\n",
                            parser_filename(p), fname.line, fname.col, fname.text);
                    p->had_error = 1;
                }
                if (symtab_find_class_const(info->name, fname.text)) {
                    fprintf(stderr, "%s(%d,%d): error: duplicate static const '%s' in class '%s'\n",
                            parser_filename(p), fname.line, fname.col, fname.text, name.text);
                    p->had_error = 1;
                }
                /* A static const may not share a name with a field or method
                   of the same class (in either declaration order; the mirror
                   checks live in the field and method branches). */
                {
                    int name_clash = symtab_find_method_in_class(info, fname.text) != NULL;
                    int fci;
                    for (fci = 0; fci < info->field_count && fci < MAX_FIELDS; fci++) {
                        if (strcmp(info->field_names[fci], fname.text) == 0) { name_clash = 1; break; }
                    }
                    if (name_clash) {
                        fprintf(stderr, "%s(%d,%d): error: static const '%s' conflicts with an existing member in class '%s'\n",
                                parser_filename(p), fname.line, fname.col, fname.text, name.text);
                        p->had_error = 1;
                    }
                }
                ConstInfo* cc = (ConstInfo*)calloc(1, sizeof(ConstInfo));
                CHECK_STRSCPY(strscpy(cc->name, fname.text, sizeof(cc->name)), "const name too long");
                CHECK_STRSCPY(strscpy(cc->owner_class, info->name, sizeof(cc->owner_class)), "class name too long");
                cc->const_type = ft;
                cc->const_is_string = static_const_string;
                cc->is_private = is_private;
                parse_const_initializer(p, fname, &ft, static_const_string, cc);
                symtab_add_const(fname.text, cc);
                expect(p, TOK_SEMI);
                continue;
            }
            if (ft.is_const) {
                fprintf(stderr, "%s(%d,%d): error: const fields are not supported\n",
                        parser_filename(p), fname.line, fname.col);
                p->had_error = 1;
            }
            if (is_static) {
                fprintf(stderr, "%s(%d,%d): error: static fields are not supported\n",
                        parser_filename(p), fname.line, fname.col);
                p->had_error = 1;
            }
            if (symtab_find_class_const(info->name, fname.text)) {
                fprintf(stderr, "%s(%d,%d): error: field '%s' conflicts with a static const in class '%s'\n",
                        parser_filename(p), fname.line, fname.col, fname.text, name.text);
                p->had_error = 1;
            }
            if (symtab_find_property(info, fname.text)) {
                fprintf(stderr, "%s(%d,%d): error: field '%s' conflicts with a property in class '%s'\n",
                        parser_filename(p), fname.line, fname.col, fname.text, name.text);
                p->had_error = 1;
            }
            expect(p, TOK_SEMI);
            if (symtab_add_field(info, fname.text, ft, is_private) != 0) {
                fprintf(stderr, "%s(%d,%d): error: too many fields in class '%s' (max %d)\n",
                        parser_filename(p), fname.line, fname.col, name.text, MAX_FIELDS);
                p->had_error = 1;
            }
        }
    }
    expect(p, TOK_RBRACE);

    parser_pop_type_params();

    AstNode* node = ast_new_node(AST_CLASS_DECL, name);
    node->ast_resolved_type.type_kind = TYPE_CLASS;
    CHECK_STRSCPY(strscpy(node->ast_resolved_type.class_name, name.text, sizeof(node->ast_resolved_type.class_name)), "class name too long");
    node->ast_resolved_type.type_id = info->type_id;
    if (methods) ast_add_child(node, methods);
    if (info->is_generic) {
        info->generic_ast = node;
        if (symtab_validate_generic_method_calls(info) > 0) {
            p->had_error = 1;
        }
    }
    return node;
}

static AstNode* parse_interface_decl(Parser* p) {
    advance(p); /* interface */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "%s(%d,%d): error: expected interface name\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token name = p->current; advance(p);
    parser_qualify_decl_name(p, &name);

    if (strcmp(name.text, "IToString") == 0) {
        fprintf(stderr, "%s(%d,%d): error: interface name '%s' is reserved for a builtin type\n",
                parser_filename(p), name.line, name.col, name.text);
        p->had_error = 1;
    }

    /* check name conflicts and reuse a pre-registered interface declaration */
    InterfaceInfo* info = symtab_find_interface(name.text);
    if (info) {
        if (info->method_count > 0) {
            fprintf(stderr, "%s(%d,%d): error: interface '%s' already defined\n",
                    parser_filename(p), name.line, name.col, name.text);
            p->had_error = 1;
        }
    } else {
        if (symtab_find_class(name.text) || symtab_find_struct(name.text)) {
            fprintf(stderr, "%s(%d,%d): error: type '%s' already defined\n",
                    parser_filename(p), name.line, name.col, name.text);
            p->had_error = 1;
        }

        info = (InterfaceInfo*)calloc(1, sizeof(InterfaceInfo));
        CHECK_STRSCPY(strscpy(info->name, name.text, sizeof(info->name)), "interface name too long");
        info->name[63] = '\0';
        info->type_id = symtab_next_type_id();

        /* register early for self-referential use (unlikely for interfaces,
           but consistent with class/struct registration) */
        symtab_add_interface(name.text, info);
    }

    expect(p, TOK_LBRACE);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (check(p, TOK_KW_PUBLIC) || check(p, TOK_KW_PRIVATE)) {
            fprintf(stderr, "%s(%d,%d): error: access modifiers are not allowed in interfaces\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            advance(p);
            continue;
        }
        Type ret_type = parse_type(p);
        if (ret_type.type_kind == TYPE_VOID) {
            /* allow void return type */
        }
        if (ret_type.is_unowned) {
            fprintf(stderr, "%s(%d,%d): error: unowned return type is not supported\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
        }

        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "%s(%d,%d): error: expected method name in interface\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            break;
        }
        Token mname = p->current; advance(p);

        expect(p, TOK_LPAREN);

        int mc = 0;
        char mpn[MAX_PARAMS][NAME_BUF_SIZE];
        Type mpt[MAX_PARAMS];
        AstNode* mpd[MAX_PARAMS];
        int seen_default = 0;

        if (!check(p, TOK_RPAREN)) {
            do {
                int is_ref = 0;
                if (check(p, TOK_KW_REF)) {
                    is_ref = 1; advance(p);
                }
                Type pt = parse_type(p);
                pt.is_ref = is_ref;
                if (is_ref && pt.is_const) {
                    fprintf(stderr, "%s(%d,%d): error: ref parameters cannot be const\n",
                            parser_filename(p), p->current.line, p->current.col);
                    p->had_error = 1;
                }
                if (!check(p, TOK_IDENT)) {
                    fprintf(stderr, "%s(%d,%d): error: expected parameter name in interface method\n",
                            parser_filename(p), p->current.line, p->current.col);
                    p->had_error = 1;
                    break;
                }
                Token pn = p->current; advance(p);
                AstNode* pdefault = NULL;
                if (check(p, TOK_ASSIGN)) {
                    pdefault = parse_param_default(p, &pt, pn.text, is_ref);
                    seen_default = 1;
                } else if (seen_default) {
                    fprintf(stderr, "%s(%d,%d): error: parameter '%s' must have a default value because a previous parameter has one\n",
                            parser_filename(p), pn.line, pn.col, pn.text);
                    p->had_error = 1;
                }
                if (mc < MAX_PARAMS) {
                    CHECK_STRSCPY(strscpy(mpn[mc], pn.text, sizeof(mpn[mc])), "parameter name too long");
                    mpt[mc] = pt;
                    mpd[mc] = pdefault;
                    mc++;
                }
            } while (check(p, TOK_COMMA) && (advance(p), 1));
        }
        expect(p, TOK_RPAREN);

        if (ret_type.is_array) {
            fprintf(stderr, "%s(%d,%d): error: interface method '%s' cannot return array by value\n",
                    parser_filename(p), mname.line, mname.col, mname.text);
            p->had_error = 1;
        }
        {
            int pi;
            for (pi = 0; pi < mc; pi++) {
                if (mpt[pi].is_array && !mpt[pi].is_ref) {
                    fprintf(stderr, "%s(%d,%d): error: array parameter '%s' of interface method '%s' must be ref\n",
                            parser_filename(p), mname.line, mname.col, mpn[pi], mname.text);
                    p->had_error = 1;
                }
            }
        }

        AstNode* default_body = NULL;
        if (check(p, TOK_LBRACE)) {
            symtab_enter_scope();
            int pi;
            for (pi = 0; pi < mc; pi++) {
                Type pt = mpt[pi];
                symtab_insert(mpn[pi], pt);
            }
            default_body = parse_stmt(p);
            symtab_exit_scope();
        } else if (check(p, TOK_SEMI)) {
            advance(p); /* ; */
        } else {
            fprintf(stderr, "%s(%d,%d): error: expected ';' or '{' after interface method signature\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            break;
        }

        if (symtab_add_interface_method(info, mname.text, ret_type, mc, mpn, mpt, mpd, default_body, mname.line) != 0) {
            fprintf(stderr, "%s(%d,%d): error: too many methods in interface '%s' (max %d)\n",
                    parser_filename(p), mname.line, mname.col, name.text, MAX_IFACE_METHODS);
            p->had_error = 1;
        }
    }

    expect(p, TOK_RBRACE);

    AstNode* node = ast_new_node(AST_INTERFACE_DECL, name);
    node->ast_resolved_type.type_kind = TYPE_INTERFACE;
    CHECK_STRSCPY(strscpy(node->ast_resolved_type.class_name, name.text,
                          sizeof(node->ast_resolved_type.class_name)),
                  "interface name too long");
    return node;
}

static AstNode* parse_func_decl(Parser* p, Type ret_type) {
    Token name = p->current; advance(p);
    if (p->ns_prefix[0] && strcmp(name.text, "main") == 0) {
        fprintf(stderr, "%s(%d,%d): error: 'main' must be declared at top level, outside any namespace\n",
                parser_filename(p), name.line, name.col);
        p->had_error = 1;
    }
    parser_qualify_decl_name(p, &name);
    expect(p, TOK_LPAREN);

    symtab_enter_scope();

    AstNode* params = NULL;
    int pc = 0;
    char pn[MAX_PARAMS][NAME_BUF_SIZE];
    Type pt[MAX_PARAMS];
    AstNode* pd[MAX_PARAMS];
    int seen_default = 0;
    if (!check(p, TOK_RPAREN)) {
        do {
            int is_ref = 0;
            if (check(p, TOK_KW_REF)) {
                is_ref = 1; advance(p);
            }
            Type param_type = parse_type(p);
            param_type.is_ref = is_ref;
            if (is_ref && param_type.is_const) {
                fprintf(stderr, "%s(%d,%d): error: ref parameters cannot be const\n",
                        parser_filename(p), p->current.line, p->current.col);
                p->had_error = 1;
            }
            if (!check(p, TOK_IDENT)) {
                fprintf(stderr, "%s(%d,%d): error: expected parameter name\n",
                        parser_filename(p), p->current.line, p->current.col);
                p->had_error = 1;
                break;
            }
            Token pname = p->current; advance(p);
            AstNode* pdefault = NULL;
            if (check(p, TOK_ASSIGN)) {
                pdefault = parse_param_default(p, &param_type, pname.text, is_ref);
                seen_default = 1;
            } else if (seen_default) {
                fprintf(stderr, "%s(%d,%d): error: parameter '%s' must have a default value because a previous parameter has one\n",
                        parser_filename(p), pname.line, pname.col, pname.text);
                p->had_error = 1;
            }
            AstNode* pdecl = ast_new_node(AST_VAR_DECL, pname);
            pdecl->ast_resolved_type = param_type;
            symtab_insert(pname.text, param_type);
            params = ast_append_list(params, pdecl);
            if (pc < MAX_PARAMS) {
                CHECK_STRSCPY(strscpy(pn[pc], pname.text, sizeof(pn[pc])), "parameter name too long");
                pt[pc] = param_type;
                pd[pc] = pdefault;
                pc++;
            }
        } while (check(p, TOK_COMMA) && (advance(p), 1));
    }
    expect(p, TOK_RPAREN);

    if (ret_type.is_array) {
        fprintf(stderr, "%s(%d,%d): error: function '%s' cannot return array by value\n",
                parser_filename(p), name.line, name.col, name.text);
        p->had_error = 1;
    }
    {
        int pi;
        for (pi = 0; pi < pc; pi++) {
            if (pt[pi].is_array && !pt[pi].is_ref) {
                fprintf(stderr, "%s(%d,%d): error: array parameter '%s' of function '%s' must be ref\n",
                        parser_filename(p), name.line, name.col, pn[pi], name.text);
                p->had_error = 1;
            }
        }
    }

    AstNode* body = parse_stmt(p);
    symtab_exit_scope();

    symtab_add_func(name.text, ret_type, pc, pn, pt, pd, 0);

    AstNode* node = ast_new_node(AST_FUNC_DECL, name);
    node->ast_resolved_type = ret_type;
    if (params) { ast_add_child(node, params); }
    ast_add_child(node, body);
    return node;
}

/* enum Key { Up, Down, Left = 10, Right } -- simple enum (C++ enum class
   style), unit variants only.  The declaration lives entirely in the symtab
   (no AST node); codegen emits the C typedef from the EnumInfo registry.
   The name was pre-registered by parser_register_forward_decls. */
static AstNode* parse_enum_decl(Parser* p) {
    advance(p); /* enum */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "%s(%d,%d): error: expected enum name\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token name = p->current; advance(p);
    parser_qualify_decl_name(p, &name);
    expect(p, TOK_LBRACE);

    EnumInfo* info = symtab_find_enum(name.text);
    int first_definition = 1;
    if (info) {
        /* variant_count > 0 means the body was already parsed once. */
        if (info->variant_count > 0) {
            fprintf(stderr, "%s(%d,%d): error: enum '%s' already defined\n",
                    parser_filename(p), name.line, name.col, name.text);
            p->had_error = 1;
            first_definition = 0;
        }
    } else {
        info = (EnumInfo*)calloc(1, sizeof(EnumInfo));
        CHECK_STRSCPY(strscpy(info->name, name.text, sizeof(info->name)), "enum name too long");
        info->name[63] = '\0';
        symtab_add_enum(name.text, info);
    }

    long next_value = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "%s(%d,%d): error: expected enum variant name\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            advance(p);
            continue;
        }
        Token variant = p->current; advance(p);

        if (check(p, TOK_LPAREN) || check(p, TOK_LBRACE)) {
            /* Tuple-style Variant(...) and struct-style Variant {...} are the
               payload-enum forms; reserved for a future language version. */
            fprintf(stderr, "%s(%d,%d): error: payload enums are not yet supported\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            /* Skip the payload, then continue with the next variant. */
            int depth = 0;
            while (!check(p, TOK_EOF)) {
                if (depth == 0 && (check(p, TOK_COMMA) || check(p, TOK_RBRACE))) break;
                if (check(p, TOK_LPAREN) || check(p, TOK_LBRACE)) depth++;
                if (check(p, TOK_RPAREN) || check(p, TOK_RBRACE)) {
                    if (depth == 0) break;
                    depth--;
                }
                advance(p);
            }
            if (check(p, TOK_COMMA)) { advance(p); }
            continue;
        }

        int duplicate = 0;
        int vi;
        for (vi = 0; vi < info->variant_count; vi++) {
            if (strcmp(info->variant_names[vi], variant.text) == 0) { duplicate = 1; break; }
        }
        if (duplicate) {
            fprintf(stderr, "%s(%d,%d): error: duplicate variant '%s' in enum '%s'\n",
                    parser_filename(p), variant.line, variant.col, variant.text, name.text);
            p->had_error = 1;
        }

        long value = next_value;
        if (check(p, TOK_ASSIGN)) {
            advance(p);
            int neg = 0;
            if (check(p, TOK_MINUS)) { neg = 1; advance(p); }
            if (!check(p, TOK_INT_LIT)) {
                fprintf(stderr, "%s(%d,%d): error: expected integer literal as enum variant value\n",
                        parser_filename(p), p->current.line, p->current.col);
                p->had_error = 1;
            } else {
                value = p->current.int_val;
                if (neg) value = -value;
                advance(p);
            }
        }
        next_value = value + 1;

        if (first_definition && !duplicate) {
            if (info->variant_count >= MAX_FIELDS) {
                fprintf(stderr, "%s(%d,%d): error: too many variants in enum '%s' (max %d)\n",
                        parser_filename(p), variant.line, variant.col, name.text, MAX_FIELDS);
                p->had_error = 1;
            } else {
                CHECK_STRSCPY(strscpy(info->variant_names[info->variant_count], variant.text,
                                      sizeof(info->variant_names[0])), "enum variant name too long");
                info->variant_values[info->variant_count] = value;
                info->variant_count++;
            }
        }

        if (check(p, TOK_COMMA)) {
            advance(p);
        } else if (!check(p, TOK_RBRACE)) {
            expect(p, TOK_COMMA);
        }
    }
    expect(p, TOK_RBRACE);

    if (first_definition && info->variant_count == 0 && !p->had_error) {
        fprintf(stderr, "%s(%d,%d): error: enum '%s' must have at least one variant\n",
                parser_filename(p), name.line, name.col, name.text);
        p->had_error = 1;
    }
    return NULL;
}

/* Parses the mandatory '= <literal>' initializer of a const declaration
   (top-level or static class member) and fills info->const_literal.
   Literals only, matching the declared type (same rule as default parameter
   values); an optional '-' may precede a numeric literal.  Does not consume
   the trailing ';'. */
static void parse_const_initializer(Parser* p, Token name, const Type* t,
                                    int is_string, ConstInfo* info) {
    if (!check(p, TOK_ASSIGN)) {
        fprintf(stderr, "%s(%d,%d): error: const '%s' requires an initializer\n",
                parser_filename(p), name.line, name.col, name.text);
        p->had_error = 1;
        return;
    }
    advance(p); /* = */

    int neg = 0;
    if (check(p, TOK_MINUS)) { neg = 1; advance(p); }

    int lit_ok = 1;

    if (is_string) {
        if (neg || !check(p, TOK_STRING_LIT)) {
            lit_ok = 0;
        } else {
            Token lit = p->current; advance(p);
            unescape_brace_sentinels(lit.text);
            CHECK_STRSCPY(strscpy(info->const_literal, lit.text, sizeof(info->const_literal)),
                          "const string literal too long");
        }
    } else if (t->type_kind == TYPE_BOOL) {
        if (neg || !(check(p, TOK_KW_TRUE) || check(p, TOK_KW_FALSE))) {
            lit_ok = 0;
        } else {
            CHECK_STRSCPY(strscpy(info->const_literal, check(p, TOK_KW_TRUE) ? "1" : "0",
                                  sizeof(info->const_literal)), "const literal too long");
            advance(p);
        }
    } else if (t->type_kind == TYPE_F32 || t->type_kind == TYPE_F64) {
        if (!check(p, TOK_INT_LIT) && !check(p, TOK_FLOAT_LIT)) {
            lit_ok = 0;
        } else {
            snprintf(info->const_literal, sizeof(info->const_literal), "%s%s",
                     neg ? "-" : "", p->current.text);
            advance(p);
        }
    } else {
        /* Integer types: integer or char literal.  Char literals store the
           decoded char in char_val; re-wrap it with C quoting/escaping. */
        if (check(p, TOK_CHAR_LIT) && !neg) {
            char c = p->current.char_val;
            const char* esc = NULL;
            switch (c) {
                case '\n': esc = "'\\n'"; break;
                case '\t': esc = "'\\t'"; break;
                case '\r': esc = "'\\r'"; break;
                case '\\': esc = "'\\\\'"; break;
                case '\'': esc = "'\\''"; break;
            }
            if (esc) {
                CHECK_STRSCPY(strscpy(info->const_literal, esc, sizeof(info->const_literal)),
                              "const literal too long");
            } else {
                snprintf(info->const_literal, sizeof(info->const_literal), "'%c'", c);
            }
            advance(p);
        } else if (check(p, TOK_INT_LIT)) {
            snprintf(info->const_literal, sizeof(info->const_literal), "%s%s",
                     neg ? "-" : "", p->current.text);
            advance(p);
        } else {
            lit_ok = 0;
        }
    }
    if (!lit_ok) {
        fprintf(stderr, "%s(%d,%d): error: const '%s' initializer must be a literal matching type '%s'\n",
                parser_filename(p), p->current.line, p->current.col, name.text, type_name(t));
        p->had_error = 1;
    }
}

/* const u32 X = 1; / const string S = "hello"; -- top-level const declaration.
   The name is inserted into the global scope (uses resolve as ordinary
   identifiers; is_const blocks reassignment) and recorded in the ConstInfo
   registry, from which codegen emits the C declaration.  Strings are allowed
   here even though local consts are primitive-only: a global string const is
   initialized once at program start and never reassigned. */
static AstNode* parse_const_decl(Parser* p) {
    advance(p); /* const */

    Type t = parse_type(p);
    int is_string = t.type_kind == TYPE_CLASS && strcmp(t.class_name, "String") == 0 &&
                    !t.is_array && !t.is_weak && !t.is_unowned;
    if (t.is_array || t.is_weak || t.is_unowned || t.is_ref ||
        (!type_is_primitive_value(t.type_kind) && !is_string)) {
        fprintf(stderr, "%s(%d,%d): error: top-level const is only supported on primitive value types and string\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
    }
    t.is_const = 1;

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "%s(%d,%d): error: expected const name\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
        expect(p, TOK_SEMI);
        return NULL;
    }
    Token name = p->current; advance(p);
    parser_qualify_decl_name(p, &name);

    if (symtab_lookup_current(name.text) || symtab_find_func(name.text) ||
        is_type_name(name.text)) {
        fprintf(stderr, "%s(%d,%d): error: const '%s' conflicts with an existing declaration\n",
                parser_filename(p), name.line, name.col, name.text);
        p->had_error = 1;
    }

    ConstInfo* info = (ConstInfo*)calloc(1, sizeof(ConstInfo));
    CHECK_STRSCPY(strscpy(info->name, name.text, sizeof(info->name)), "const name too long");
    info->const_type = t;
    info->const_is_string = is_string;
    parse_const_initializer(p, name, &t, is_string, info);

    symtab_add_const(name.text, info);
    symtab_insert(name.text, t);
    expect(p, TOK_SEMI);
    return NULL;
}

/* namespace N { ... } -- one level of name grouping.  Declarations inside
   are registered under the underscored name "N_name" (see
   parser_qualify_decl_name), so the rest of the compiler treats them as
   ordinary top-level declarations.  Nested namespaces are rejected. */
static AstNode* parse_namespace_decl(Parser* p) {
    advance(p); /* namespace */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "%s(%d,%d): error: expected namespace name\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token nsname = p->current; advance(p);
    symtab_add_namespace(nsname.text);

    if (p->ns_prefix[0]) {
        fprintf(stderr, "%s(%d,%d): error: nested namespaces are not supported\n",
                parser_filename(p), nsname.line, nsname.col);
        p->had_error = 1;
        /* Parse the body under the outer prefix so the file still compiles
           far enough to report further errors. */
    }
    expect(p, TOK_LBRACE);

    char saved[NAME_BUF_SIZE];
    CHECK_STRSCPY(strscpy(saved, p->ns_prefix, sizeof(saved)), "namespace prefix too long");
    if (!p->ns_prefix[0]) {
        CHECK_STRSCPY(strscpy(p->ns_prefix, nsname.text, sizeof(p->ns_prefix)),
                      "namespace name too long");
    }

    AstNode* decls = NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        AstNode* d = parse_top_level(p);
        if (d) decls = ast_append_list(decls, d);
    }
    expect(p, TOK_RBRACE);

    CHECK_STRSCPY(strscpy(p->ns_prefix, saved, sizeof(p->ns_prefix)), "namespace prefix too long");
    return decls;
}

static AstNode* parse_top_level(Parser* p) {
    if (check(p, TOK_KW_NAMESPACE)) {
        return parse_namespace_decl(p);
    }

    if (check(p, TOK_KW_CLASS)) {
        return parse_class_decl(p);
    }

    if (check(p, TOK_KW_CONST)) {
        return parse_const_decl(p);
    }

    if (check(p, TOK_KW_STRUCT)) {
        return parse_struct_decl(p);
    }

    if (check(p, TOK_KW_ENUM)) {
        return parse_enum_decl(p);
    }

    if (check(p, TOK_KW_INTERFACE)) {
        return parse_interface_decl(p);
    }

    if (is_type_token(p) ||
        (check(p, TOK_IDENT) && is_type_name(p->current.text)) ||
        check(p, TOK_KW_UNOWNED) ||
        (check(p, TOK_IDENT) && strcmp(p->current.text, "void") == 0)) {
        Type ret_type = parse_type(p);
        if (ret_type.is_unowned) {
            fprintf(stderr, "%s(%d,%d): error: unowned return type is not supported\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
        }
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "%s(%d,%d): error: expected function name\n",
                    parser_filename(p), p->current.line, p->current.col);
            p->had_error = 1;
            advance(p);
            return NULL;
        }
        return parse_func_decl(p, ret_type);
    }

    fprintf(stderr, "%s(%d,%d): error: unexpected token '%s' at top level\n",
            parser_filename(p), p->current.line, p->current.col, p->current.text);
    p->had_error = 1;
    advance(p);
    return NULL;
}


void parser_init(Parser* p, Lexer* lexer) {
    p->lexer     = lexer;
    p->had_error = 0;
    p->ns_prefix[0] = '\0';
    advance(p);
    advance(p);
    advance(p);
}

/* Pre-register all top-level class, struct, and interface names so that
   mutually-referencing types (e.g., SdlWindow holding an SdlApp field while
   SdlApp is defined later) can resolve without forward-declaration syntax.
   Declarations inside a namespace block are registered under the qualified
   underscored name "N_name", matching parser_qualify_decl_name. */
static void parser_register_forward_decls(Parser* p) {
    Lexer scan = *p->lexer;
    char fwd_prefix[NAME_BUF_SIZE] = "";
    int  fwd_depth = 0;
    Token cur = lexer_next(&scan);
    while (cur.kind != TOK_EOF) {
        if (cur.kind == TOK_KW_NAMESPACE) {
            Token nsname = lexer_next(&scan);
            if (nsname.kind == TOK_IDENT) {
                symtab_add_namespace(nsname.text);
            }
            Token brace = lexer_next(&scan);
            if (brace.kind == TOK_LBRACE) {
                if (fwd_depth == 0 && nsname.kind == TOK_IDENT) {
                    CHECK_STRSCPY(strscpy(fwd_prefix, nsname.text, sizeof(fwd_prefix)),
                                  "namespace name too long");
                }
                /* Nested namespaces are a parser error; the scan just keeps
                   the outer prefix so braces still pair up. */
                fwd_depth++;
            }
            cur = lexer_next(&scan);
            continue;
        }
        if (fwd_depth > 0) {
            if (cur.kind == TOK_LBRACE) {
                fwd_depth++;
            } else if (cur.kind == TOK_RBRACE) {
                fwd_depth--;
                if (fwd_depth == 0) fwd_prefix[0] = '\0';
            }
        }
        if (cur.kind == TOK_KW_CLASS || cur.kind == TOK_KW_STRUCT || cur.kind == TOK_KW_INTERFACE ||
            cur.kind == TOK_KW_ENUM) {
            TokenKind decl_kind = cur.kind;
            Token name = lexer_next(&scan);
            char qname[NAME_BUF_SIZE];
            const char* regname = name.text;
            if (fwd_prefix[0]) {
                int qn = snprintf(qname, sizeof(qname), "%s_%s", fwd_prefix, name.text);
                CHECK_SNPRINTF(qn, sizeof(qname), "qualified name too long");
                regname = qname;
            }
            if (name.kind == TOK_IDENT) {
                if (decl_kind == TOK_KW_CLASS) {
                    if (!symtab_find_class(regname)) {
                        ClassInfo* info = (ClassInfo*)calloc(1, sizeof(ClassInfo));
                        CHECK_STRSCPY(strscpy(info->name, regname, sizeof(info->name)), "class name too long");
                        info->name[63] = '\0';
                        Token t = lexer_next(&scan);
                        if (t.kind == TOK_LT) {
                            char params[MAX_GENERIC_PARAMS][64];
                            int param_count = 0;
                            Token tok = lexer_next(&scan);
                            while (tok.kind != TOK_GT && tok.kind != TOK_EOF) {
                                if (tok.kind == TOK_IDENT && param_count < MAX_GENERIC_PARAMS) {
                                    CHECK_STRSCPY(strscpy(params[param_count], tok.text, sizeof(params[param_count])), "generic parameter name too long");
                                    param_count++;
                                }
                                tok = lexer_next(&scan);
                                if (tok.kind == TOK_COMMA) {
                                    tok = lexer_next(&scan);
                                }
                            }
                            if (param_count > 0) {
                                const char* param_ptrs[MAX_GENERIC_PARAMS];
                                int k;
                                for (k = 0; k < param_count && k < MAX_GENERIC_PARAMS; k++) {
                                    param_ptrs[k] = params[k];
                                }
                                symtab_mark_class_generic(info, NULL, param_count, param_ptrs);
                            }
                        }
                        if (!info->is_generic) {
                            info->type_id = symtab_next_type_id();
                        }
                        symtab_add_class(regname, info);
                    }
                } else if (decl_kind == TOK_KW_STRUCT) {
                    if (!symtab_find_struct(regname)) {
                        StructInfo* info = (StructInfo*)calloc(1, sizeof(StructInfo));
                        CHECK_STRSCPY(strscpy(info->name, regname, sizeof(info->name)), "struct name too long");
                        info->name[63] = '\0';
                        info->type_id = symtab_next_type_id() | TYPE_IS_STRUCT;
                        symtab_add_struct(regname, info);
                    }
                } else if (decl_kind == TOK_KW_INTERFACE) {
                    if (!symtab_find_interface(regname)) {
                        InterfaceInfo* info = (InterfaceInfo*)calloc(1, sizeof(InterfaceInfo));
                        CHECK_STRSCPY(strscpy(info->name, regname, sizeof(info->name)), "interface name too long");
                        info->name[63] = '\0';
                        info->type_id = symtab_next_type_id();
                        symtab_add_interface(regname, info);
                    }
                } else if (decl_kind == TOK_KW_ENUM) {
                    /* Enums have no generics and no dependencies; only the
                       name is pre-registered.  parse_enum_decl fills in the
                       variants when the body is parsed. */
                    if (!symtab_find_enum(regname)) {
                        EnumInfo* info = (EnumInfo*)calloc(1, sizeof(EnumInfo));
                        CHECK_STRSCPY(strscpy(info->name, regname, sizeof(info->name)), "enum name too long");
                        info->name[63] = '\0';
                        symtab_add_enum(regname, info);
                    }
                }
            }
        }
        cur = lexer_next(&scan);
    }
}

/* ================================================================
   IMPORTS
   import("path/to/file.my") merges the referenced file's top-level
   declarations into the current compilation unit (whole-program
   compilation).  Paths are resolved relative to the importing file,
   canonicalized, and parsed at most once, so diamond imports and
   import cycles are handled by dedup rather than by error.
   ================================================================ */

#define MAX_IMPORT_FILES 64
#define MAX_IMPORT_DEPTH 16

static char g_import_paths[MAX_IMPORT_FILES][1024];
static int  g_import_path_count = 0;

static void import_normalize_seps(char* s) {
    for (; *s; s++) { if (*s == '\\') *s = '/'; }
}

static int import_paths_equal(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

/* Resolve spec relative to the requesting file's directory and
   canonicalize to an absolute path with forward slashes. */
static void import_resolve_path(const char* requester, const char* spec,
                                char* out, size_t outsz) {
    char joined[1024];
    int is_absolute = (spec[0] == '/' || spec[0] == '\\' ||
                       (isalpha((unsigned char)spec[0]) && spec[1] == ':'));
    if (is_absolute) {
        int n = snprintf(joined, sizeof(joined), "%s", spec);
        CHECK_SNPRINTF(n, sizeof(joined), "import path too long");
    } else {
        char dir[1024];
        int n = snprintf(dir, sizeof(dir), "%s", requester ? requester : "");
        CHECK_SNPRINTF(n, sizeof(dir), "import path too long");
        import_normalize_seps(dir);
        char* slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; } else { dir[0] = '\0'; }
        if (dir[0] != '\0') {
            n = snprintf(joined, sizeof(joined), "%s/%s", dir, spec);
        } else {
            n = snprintf(joined, sizeof(joined), "%s", spec);
        }
        CHECK_SNPRINTF(n, sizeof(joined), "import path too long");
    }
#ifdef _WIN32
    if (_fullpath(out, joined, outsz) == NULL) {
        int n = snprintf(out, outsz, "%s", joined);
        CHECK_SNPRINTF(n, outsz, "import path too long");
    }
#else
    {
        char* rp = realpath(joined, NULL);
        if (rp) {
            int n = snprintf(out, outsz, "%s", rp);
            CHECK_SNPRINTF(n, outsz, "import path too long");
            free(rp);
        } else {
            int n = snprintf(out, outsz, "%s", joined);
            CHECK_SNPRINTF(n, outsz, "import path too long");
        }
    }
#endif
    import_normalize_seps(out);
}

static char* import_read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
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

static AstNode* parser_parse_file_decls(Parser* p, int depth);

/* Parse import("path") and splice the imported file's declarations into
   the current declaration list.  Returns the imported decls (or NULL). */
static AstNode* parser_parse_import(Parser* p, int depth) {
    Token kw = p->current;   /* the 'import' identifier, for error positions */
    advance(p);              /* import */
    advance(p);              /* ( */
    if (!check(p, TOK_STRING_LIT)) {
        fprintf(stderr, "%s(%d,%d): error: expected string literal path after 'import('\n",
                parser_filename(p), p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token path_tok = p->current; advance(p);
    expect(p, TOK_RPAREN);
    if (check(p, TOK_SEMI)) advance(p);

    if (depth >= MAX_IMPORT_DEPTH) {
        fprintf(stderr, "%s(%d,%d): error: import depth exceeds %d\n",
                parser_filename(p), kw.line, kw.col, MAX_IMPORT_DEPTH);
        p->had_error = 1;
        return NULL;
    }

    char resolved[1024];
    import_resolve_path(parser_filename(p), path_tok.text, resolved, sizeof(resolved));

    int i;
    for (i = 0; i < g_import_path_count; i++) {
        if (import_paths_equal(g_import_paths[i], resolved)) {
            return NULL;   /* already parsed: diamond import or cycle */
        }
    }
    if (g_import_path_count >= MAX_IMPORT_FILES) {
        fprintf(stderr, "%s(%d,%d): error: too many imported files (max %d)\n",
                parser_filename(p), kw.line, kw.col, MAX_IMPORT_FILES);
        p->had_error = 1;
        return NULL;
    }
    /* Register before parsing so import cycles terminate by dedup.  The
       stored path doubles as the sub-lexer's filename: tokens keep the
       pointer, so it must outlive this function (a local buffer would
       dangle by the time codegen reads token filenames). */
    CHECK_STRSCPY(strscpy(g_import_paths[g_import_path_count], resolved,
                          sizeof(g_import_paths[0])), "import path too long");
    const char* stored_path = g_import_paths[g_import_path_count];
    g_import_path_count++;

    char* src = import_read_file(resolved);
    if (!src) {
        fprintf(stderr, "%s(%d,%d): error: cannot open imported file '%s' (resolved to '%s')\n",
                parser_filename(p), path_tok.line, path_tok.col, path_tok.text, resolved);
        p->had_error = 1;
        return NULL;
    }

    Lexer sub_lexer;
    lexer_init(&sub_lexer, src);
    lexer_set_filename(&sub_lexer, stored_path);
    Parser sub;
    parser_init(&sub, &sub_lexer);
    AstNode* decls = parser_parse_file_decls(&sub, depth + 1);
    if (sub.had_error) p->had_error = 1;

    /* Only the root file may define main. */
    {
        AstNode* d = decls;
        while (d) {
            if (d->ast_kind == AST_FUNC_DECL && strcmp(d->ast_token.text, "main") == 0) {
                fprintf(stderr, "%s(%d,%d): error: imported file '%s' must not define 'main'\n",
                        parser_filename(p), kw.line, kw.col, path_tok.text);
                p->had_error = 1;
            }
            d = d->next;
        }
    }
    return decls;
}

static int at_import_directive(Parser* p) {
    return check(p, TOK_IDENT) && strcmp(p->current.text, "import") == 0 &&
           p->peek.kind == TOK_LPAREN;
}

static AstNode* parser_parse_file_decls(Parser* p, int depth) {
    /* Register all class/struct/interface names first, so fields and method
       signatures can refer to types that are defined later in the source. */
    parser_register_forward_decls(p);

    AstNode* decls = NULL;
    while (!check(p, TOK_EOF)) {
        if (at_import_directive(p)) {
            AstNode* imported = parser_parse_import(p, depth);
            while (imported) {
                AstNode* nxt = imported->next;
                imported->next = NULL;
                decls = ast_append_list(decls, imported);
                imported = nxt;
            }
            continue;
        }
        AstNode* d = parse_top_level(p);
        if (d) decls = ast_append_list(decls, d);
    }
    return decls;
}

AstNode* parser_parse_program(Parser* p) {
    Type void_type;
    memset(&void_type, 0, sizeof(void_type));
    void_type.type_kind = TYPE_VOID;

    AstNode* program = ast_new_node(AST_PROGRAM, p->current);
    program->ast_resolved_type = void_type;

    symtab_init();

    /* Reset the import dedup set and register the root file so an import
       cycle back to it terminates. */
    g_import_path_count = 0;
    {
        char root[1024];
        import_resolve_path("", parser_filename(p), root, sizeof(root));
        CHECK_STRSCPY(strscpy(g_import_paths[0], root, sizeof(g_import_paths[0])),
                      "import path too long");
        g_import_path_count = 1;
    }

    AstNode* decls = parser_parse_file_decls(p, 0);

    ast_add_child(program, decls);
    return program;
}
int parser_had_error(Parser* p) { return p->had_error; }
