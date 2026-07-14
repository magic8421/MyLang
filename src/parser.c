#include "parser.h"
#include "symtab.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
    fprintf(stderr, "error at %d:%d: expected '%s', got '%s'\n",
            p->current.line, p->current.col,
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

static int is_type_name(const char* name) {
    return symtab_find_class(name) != NULL ||
           symtab_find_struct(name) != NULL ||
           symtab_find_interface(name) != NULL;
}

/* ================================================================
   FORWARD DECLARATIONS
   ================================================================ */

static AstNode* parse_stmt(Parser* p);
static AstNode* parse_expr(Parser* p);
static AstNode* parse_expr_no_assign(Parser* p, const char* where);



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
    } else if (check(p, TOK_IDENT) && strcmp(p->current.text, "void") == 0) {
        advance(p);
        t.type_kind = TYPE_VOID;
    } else if (check(p, TOK_IDENT)) {
        const char* name = p->current.text;
        if (parser_is_type_param(name)) {
            Type t = type_make_param(name);
            advance(p);
            return t;
        }
        ClassInfo* ci = symtab_find_class(name);
        StructInfo* si = symtab_find_struct(name);
        InterfaceInfo* ii = symtab_find_interface(name);
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
        } else {
            fprintf(stderr, "error at %d:%d: unknown type '%s'\n",
                    p->current.line, p->current.col, name);
            p->had_error = 1;
            t.type_kind = TYPE_VOID;
        }
        advance(p);
    } else {
        fprintf(stderr, "error at %d:%d: expected type\n",
                p->current.line, p->current.col);
        p->had_error = 1;
        t.type_kind = TYPE_VOID;
    }

    if (check(p, TOK_LT)) {
        ClassInfo* ci = symtab_find_class(t.class_name);
        if (t.type_kind != TYPE_CLASS || !ci || !ci->is_generic) {
            fprintf(stderr, "error at %d:%d: type '%s' does not accept type arguments\n",
                    p->current.line, p->current.col, t.class_name);
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
                    fprintf(stderr, "error at %d:%d: too many type arguments for '%s' (max %d)\n",
                            p->current.line, p->current.col, t.class_name, MAX_TYPE_ARGS);
                    p->had_error = 1;
                }
                count++;
            } while (check(p, TOK_COMMA) && (advance(p), 1));
        }
        expect(p, TOK_GT);
        t.type_id = 0;
        type_mangled_name(&t);
    }

    return t;
}

static Type parse_type(Parser* p) {
    int is_weak = 0;
    if (check(p, TOK_KW_WEAK)) {
        is_weak = 1;
        advance(p);
    }

    Type t = parse_base_type(p);

    if (is_weak) {
        if (t.type_kind != TYPE_CLASS && t.type_kind != TYPE_INTERFACE) {
            fprintf(stderr, "error at %d:%d: weak requires a class or interface type\n",
                    p->current.line, p->current.col);
            p->had_error = 1;
        } else {
            t.is_weak = 1;
            t.mangled_name[0] = '\0';
        }
    }

    if (check(p, TOK_LBRACKET)) {
        advance(p);
        t.is_array = 1;
        if (check(p, TOK_RBRACKET)) {
            advance(p);
            t.is_pointer = 1;
        } else if (check(p, TOK_INT_LIT)) {
            t.array_size = p->current.int_val;
            advance(p);
            expect(p, TOK_RBRACKET);
        } else {
            expect(p, TOK_RBRACKET);
        }
        t.mangled_name[0] = '\0';
    }
    return t;
}


static AstNode* parse_primary(Parser* p) {
    if (check(p, TOK_INT_LIT)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_INT_LIT, t);
        n->ast_resolved_type.type_kind = TYPE_I32;
        n->ast_resolved_type.type_id = TYPE_ID_I32;
        return n;
    }
    if (check(p, TOK_CHAR_LIT)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_CHAR_LIT, t);
        n->ast_resolved_type.type_kind = TYPE_I8;
        n->ast_resolved_type.type_id = TYPE_ID_I8;
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

        Type base = parse_base_type(p);
        if (base.type_kind == TYPE_VOID) {
            fprintf(stderr, "error at %d:%d: expected type after 'new'\n",
                    new_tok.line, new_tok.col);
            p->had_error = 1;
            return NULL;
        }

        AstNode* node = ast_new_node(AST_NEW, new_tok);
        node->ast_resolved_type = base;

        if (check(p, TOK_LBRACKET)) {
            advance(p);
            node->ast_children[0] = parse_expr_no_assign(p, "new array size");
            node->ast_child_count = 1;
            expect(p, TOK_RBRACKET);
        } else {
            if (base.type_kind == TYPE_INTERFACE) {
                fprintf(stderr, "error at %d:%d: cannot create instance of interface '%s'\n",
                        new_tok.line, new_tok.col, base.class_name);
                p->had_error = 1;
            } else if (base.type_kind == TYPE_STRUCT) {
                fprintf(stderr, "error at %d:%d: cannot use 'new' on a struct value; use 'new %s[N]' for an array\n",
                        new_tok.line, new_tok.col, base.class_name);
                p->had_error = 1;
            }
        }
        return node;
    }

    fprintf(stderr, "error at %d:%d: unexpected token '%s' in expression\n",
            p->current.line, p->current.col, p->current.text);
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
                fprintf(stderr, "error at %d:%d: expected field name after '.'\n",
                        p->current.line, p->current.col);
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
                args = ast_append_list(args, parse_expr_no_assign(p, "call argument"));
                while (check(p, TOK_COMMA)) {
                    advance(p);
                    args = ast_append_list(args, parse_expr_no_assign(p, "call argument"));
                }
                call->ast_children[1] = args;
                call->ast_child_count = 2;
            }
            expect(p, TOK_RPAREN);
            node = call;
        } else {
            break;
        }
    }

    if (check(p, TOK_KW_AS)) {
        Token t = p->current; advance(p);
        Type target = parse_type(p);
        if (target.type_kind == TYPE_VOID && !p->had_error) {
            fprintf(stderr, "error at %d:%d: expected type name after 'as'\n",
                    p->current.line, p->current.col);
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
    if (check(p, TOK_MINUS) || check(p, TOK_NOT)) {
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
static AstNode* parse_relational(Parser* p)     { return parse_binary(p, parse_additive,         TOK_LT, TOK_LE, TOK_GT); }
/* TOK_GE doesn't fit in parse_binary(3 ops) ??handle inline below */

static AstNode* parse_relational_full(Parser* p) {
    AstNode* left = parse_additive(p);
    if (!left) return NULL;
    while (check(p, TOK_LT) || check(p, TOK_LE) || check(p, TOK_GT) || check(p, TOK_GE)) {
        Token op = p->current; advance(p);
        AstNode* right = parse_additive(p);
        AstNode* bin = ast_new_node(AST_BINARY, op);
        ast_add_child(bin, left);
        ast_add_child(bin, right);
        left = bin;
    }
    return left;
}

static AstNode* parse_equality(Parser* p)       { return parse_binary(p, parse_relational_full, TOK_EQ, TOK_NE, (TokenKind)0); }
static AstNode* parse_logical_and(Parser* p)    { return parse_binary(p, parse_equality,        TOK_AND, (TokenKind)0, (TokenKind)0); }
static AstNode* parse_logical_or(Parser* p)     { return parse_binary(p, parse_logical_and,     TOK_OR, (TokenKind)0, (TokenKind)0); }

static AstNode* parse_assignment(Parser* p) {
    AstNode* left = parse_logical_or(p);
    if (!left) return NULL;
    if (check(p, TOK_ASSIGN)) {
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
    if (e && expr_contains_assign(e)) {
        fprintf(stderr, "error at %d:%d: assignment not allowed in %s\n",
                e->ast_token.line, e->ast_token.col, where);
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
    block->ast_children[0] = stmts;
    block->ast_child_count = 1;
    return block;
}

static int is_primitive_type_token(Parser* p) {
    return check(p, TOK_KW_U8)   || check(p, TOK_KW_U16)  ||
           check(p, TOK_KW_U32)  || check(p, TOK_KW_U64)  ||
           check(p, TOK_KW_I8)   || check(p, TOK_KW_I16)  ||
           check(p, TOK_KW_I32)  || check(p, TOK_KW_I64)  ||
           check(p, TOK_KW_F32)  || check(p, TOK_KW_F64);
}

static int stmt_looks_like_var_decl(Parser* p) {
    if (check(p, TOK_KW_WEAK)) return 1;
    if (is_primitive_type_token(p)) return 1;
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
        fprintf(stderr, "error at %d:%d: expected variable name\n",
                p->current.line, p->current.col);
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
        if (init && !p->had_error && !expr_is_direct_assignment(init) && expr_contains_assign(init)) {
            fprintf(stderr, "error at %d:%d: assignment not allowed in variable initializer\n",
                    init->ast_token.line, init->ast_token.col);
            p->had_error = 1;
        }
        if (init && init->ast_kind == AST_NEW && type.type_kind == TYPE_CLASS) {
            type.is_pointer = 1;
        }
    }

    node->ast_resolved_type = type;
    symtab_insert(name.text, type);
    expect(p, TOK_SEMI);
    return node;
}

static AstNode* parse_stmt(Parser* p) {
    if (check(p, TOK_LBRACE)) {
        return parse_block(p);
    }

    if (check(p, TOK_KW_IF)) {
        Token kw = p->current; advance(p);
        expect(p, TOK_LPAREN);
        AstNode* cond = parse_expr(p);
        if (expr_contains_assign(cond)) {
            fprintf(stderr, "error at %d:%d: assignment not allowed in if condition\n",
                    cond->ast_token.line, cond->ast_token.col);
            p->had_error = 1;
        }
        expect(p, TOK_RPAREN);
        AstNode* then_body = parse_stmt(p);
        AstNode* else_body = NULL;
        if (check(p, TOK_KW_ELSE)) {
            advance(p);
            else_body = parse_stmt(p);
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
        if (expr_contains_assign(cond)) {
            fprintf(stderr, "error at %d:%d: assignment not allowed in while condition\n",
                    cond->ast_token.line, cond->ast_token.col);
            p->had_error = 1;
        }
        expect(p, TOK_RPAREN);
        AstNode* body = parse_stmt(p);
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

    if (stmt_looks_like_var_decl(p)) {
        return parse_var_decl(p);
    }

    /* expression statement */
    if (!check(p, TOK_EOF) && !check(p, TOK_RBRACE)) {
        AstNode* expr = parse_expr(p);
        if (expr && !p->had_error && !expr_is_direct_assignment(expr) && expr_contains_assign(expr)) {
            fprintf(stderr, "error at %d:%d: assignment not allowed in expression statement\n",
                    expr->ast_token.line, expr->ast_token.col);
            p->had_error = 1;
        }
        if (expr) {
            AstNode* es = ast_new_node(AST_EXPR_STMT, expr->ast_token);
            ast_add_child(es, expr);
            expect(p, TOK_SEMI);
            return es;
        }
    }

    return NULL;
}


static AstNode* parse_struct_decl(Parser* p) {
    advance(p); /* struct */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "error at %d:%d: expected struct name\n",
                p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token name = p->current; advance(p);
    expect(p, TOK_LBRACE);

    StructInfo* info = calloc(1, sizeof(StructInfo));
    CHECK_STRSCPY(strscpy(info->name, name.text, sizeof(info->name)), "struct name too long");
    info->name[63] = '\0';
    info->type_id = symtab_next_type_id() | TYPE_IS_STRUCT;

    /* register early so later types can refer to it */
    symtab_add_struct(name.text, info);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Type ft = parse_type(p);
        if (ft.type_kind == TYPE_VOID || ft.type_kind == TYPE_CLASS || ft.type_kind == TYPE_STRUCT ||
            ft.is_array || ft.array_size > 0) {
            fprintf(stderr, "error at %d:%d: struct fields must be primitive types in this phase\n",
                    p->current.line, p->current.col);
            p->had_error = 1;
            /* try to recover */
            if (check(p, TOK_IDENT)) advance(p);
            expect(p, TOK_SEMI);
            continue;
        }
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "error at %d:%d: expected field name\n",
                    p->current.line, p->current.col);
            p->had_error = 1;
            break;
        }
        Token fname = p->current; advance(p);
        symtab_add_struct_field(info, fname.text, ft);
        expect(p, TOK_SEMI);
    }
    expect(p, TOK_RBRACE);

    AstNode* node = ast_new_node(AST_STRUCT_DECL, name);
    node->ast_resolved_type.type_kind = TYPE_STRUCT;
    CHECK_STRSCPY(strscpy(node->ast_resolved_type.class_name, name.text, sizeof(node->ast_resolved_type.class_name)), "struct name too long");
    node->ast_resolved_type.type_id = info->type_id;
    return node;
}


static AstNode* parse_class_decl(Parser* p) {
    advance(p); /* class */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "error at %d:%d: expected class name\n",
                p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token name = p->current; advance(p);

    ClassInfo* info = calloc(1, sizeof(ClassInfo));
    CHECK_STRSCPY(strscpy(info->name, name.text, sizeof(info->name)), "class name too long");
    info->name[63] = '\0';

    /* register early so methods with this return type resolve */
    symtab_add_class(name.text, info);

    /* parse optional generic parameter list */
    if (check(p, TOK_LT)) {
        advance(p); /* < */
        char params[MAX_GENERIC_PARAMS][64];
        int param_count = 0;
        do {
            if (!check(p, TOK_IDENT)) {
                fprintf(stderr, "error at %d:%d: expected type parameter name\n",
                        p->current.line, p->current.col);
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
                            fprintf(stderr, "error at %d:%d: unknown interface constraint '%s'\n",
                                    c.line, c.col, c.text);
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
                        fprintf(stderr, "error at %d:%d: expected 'new()' or interface name in constraint\n",
                                p->current.line, p->current.col);
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
                fprintf(stderr, "error at %d:%d: expected interface name after ':'\n",
                        p->current.line, p->current.col);
                p->had_error = 1;
                break;
            }
            Token iface_tok = p->current; advance(p);
            symtab_add_class_impl(info, iface_tok.text);
        } while (check(p, TOK_COMMA) && (advance(p), 1));
    }

    /* generic definitions do not get a concrete type_id until instantiated */
    if (!info->is_generic) {
        info->type_id = symtab_next_type_id();
    }

    expect(p, TOK_LBRACE);

    parser_push_type_params(info);

    AstNode* methods = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Type ft = parse_type(p);
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "error at %d:%d: expected field or method name\n",
                    p->current.line, p->current.col);
            p->had_error = 1;
            break;
        }
        Token fname = p->current; advance(p);

        if (check(p, TOK_LPAREN)) {
            /* METHOD */
            advance(p); /* consume ( */

            symtab_enter_scope();

            /* register implicit this */
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

            AstNode* mparams = NULL;
            int mc = 0;
            char mpn[16][64];
            Type mpt[16];

            if (!check(p, TOK_RPAREN)) {
                do {
                    int is_ref = 0;
                    if (check(p, TOK_KW_REF)) {
                        is_ref = 1; advance(p);
                    }
                    Type pt = parse_type(p);
                    pt.is_ref = is_ref;
                    if (!check(p, TOK_IDENT)) {
                        fprintf(stderr, "error at %d:%d: expected parameter name\n",
                                p->current.line, p->current.col);
                        p->had_error = 1;
                        break;
                    }
                    Token pn = p->current; advance(p);
                    AstNode* pd = ast_new_node(AST_VAR_DECL, pn);
                    pd->ast_resolved_type = pt;
                    symtab_insert(pn.text, pt);
                    mparams = ast_append_list(mparams, pd);
                    if (mc < 16) {
                        CHECK_STRSCPY(strscpy(mpn[mc], pn.text, sizeof(mpn[mc])), "parameter name too long");
                        mpt[mc] = pt;
                        mc++;
                    }
                } while (check(p, TOK_COMMA) && (advance(p), 1));
            }
            expect(p, TOK_RPAREN);

            AstNode* mbody = parse_stmt(p);

            symtab_exit_scope();

            symtab_add_method(info, fname.text, ft, mc, mpn, mpt);

            AstNode* mnode = ast_new_node(AST_FUNC_DECL, fname);
            mnode->ast_resolved_type = ft;
            if (mparams) { mnode->ast_children[mnode->ast_child_count++] = mparams; }
            mnode->ast_children[mnode->ast_child_count++] = mbody;
            methods = ast_append_list(methods, mnode);

        } else {
            /* FIELD */
            if (ft.type_kind == TYPE_INTERFACE) {
                fprintf(stderr, "error at %d:%d: class fields cannot have interface type '%s'\n",
                        fname.line, fname.col, ft.class_name);
                p->had_error = 1;
            }
            expect(p, TOK_SEMI);
            symtab_add_field(info, fname.text, ft);
        }
    }
    expect(p, TOK_RBRACE);

    parser_pop_type_params();

    AstNode* node = ast_new_node(AST_CLASS_DECL, name);
    node->ast_resolved_type.type_kind = TYPE_CLASS;
    CHECK_STRSCPY(strscpy(node->ast_resolved_type.class_name, name.text, sizeof(node->ast_resolved_type.class_name)), "class name too long");
    node->ast_resolved_type.type_id = info->type_id;
    node->ast_children[0] = methods;
    if (methods) node->ast_child_count = 1;
    if (info->is_generic) {
        info->generic_ast = node;
    }
    return node;
}

static AstNode* parse_interface_decl(Parser* p) {
    advance(p); /* interface */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "error at %d:%d: expected interface name\n",
                p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token name = p->current; advance(p);

    /* check name conflicts */
    if (symtab_find_class(name.text) || symtab_find_struct(name.text) ||
        symtab_find_interface(name.text)) {
        fprintf(stderr, "error at %d:%d: type '%s' already defined\n",
                p->current.line, p->current.col, name.text);
        p->had_error = 1;
    }

    expect(p, TOK_LBRACE);

    InterfaceInfo* info = calloc(1, sizeof(InterfaceInfo));
    CHECK_STRSCPY(strscpy(info->name, name.text, sizeof(info->name)), "interface name too long");
    info->name[63] = '\0';
    info->type_id = symtab_next_type_id();

    /* register early for self-referential use (unlikely for interfaces,
       but consistent with class/struct registration) */
    symtab_add_interface(name.text, info);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Type ret_type = parse_type(p);
        if (ret_type.type_kind == TYPE_VOID) {
            /* allow void return type */
        }

        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "error at %d:%d: expected method name in interface\n",
                    p->current.line, p->current.col);
            p->had_error = 1;
            break;
        }
        Token mname = p->current; advance(p);

        expect(p, TOK_LPAREN);

        int mc = 0;
        char mpn[16][64];
        Type mpt[16];

        if (!check(p, TOK_RPAREN)) {
            do {
                int is_ref = 0;
                if (check(p, TOK_KW_REF)) {
                    is_ref = 1; advance(p);
                }
                Type pt = parse_type(p);
                pt.is_ref = is_ref;
                if (!check(p, TOK_IDENT)) {
                    fprintf(stderr, "error at %d:%d: expected parameter name in interface method\n",
                            p->current.line, p->current.col);
                    p->had_error = 1;
                    break;
                }
                Token pn = p->current; advance(p);
                if (mc < 16) {
                    CHECK_STRSCPY(strscpy(mpn[mc], pn.text, sizeof(mpn[mc])), "parameter name too long");
                    mpt[mc] = pt;
                    mc++;
                }
            } while (check(p, TOK_COMMA) && (advance(p), 1));
        }
        expect(p, TOK_RPAREN);
        expect(p, TOK_SEMI);

        symtab_add_interface_method(info, mname.text, ret_type, mc, mpn, mpt);
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
    expect(p, TOK_LPAREN);

    symtab_enter_scope();

    AstNode* params = NULL;
    int pc = 0;
    char pn[16][64];
    Type pt[16];
    if (!check(p, TOK_RPAREN)) {
        do {
            int is_ref = 0;
            if (check(p, TOK_KW_REF)) {
                is_ref = 1; advance(p);
            }
            Type param_type = parse_type(p);
            param_type.is_ref = is_ref;
            if (!check(p, TOK_IDENT)) {
                fprintf(stderr, "error at %d:%d: expected parameter name\n",
                        p->current.line, p->current.col);
                p->had_error = 1;
                break;
            }
            Token pname = p->current; advance(p);
            AstNode* pd = ast_new_node(AST_VAR_DECL, pname);
            pd->ast_resolved_type = param_type;
            symtab_insert(pname.text, param_type);
            params = ast_append_list(params, pd);
            if (pc < 16) {
                CHECK_STRSCPY(strscpy(pn[pc], pname.text, sizeof(pn[pc])), "parameter name too long");
                pt[pc] = param_type;
                pc++;
            }
        } while (check(p, TOK_COMMA) && (advance(p), 1));
    }
    expect(p, TOK_RPAREN);

    AstNode* body = parse_stmt(p);
    symtab_exit_scope();

    symtab_add_func(name.text, ret_type, pc, pn, pt);

    AstNode* node = ast_new_node(AST_FUNC_DECL, name);
    node->ast_resolved_type = ret_type;
    if (params) { node->ast_children[node->ast_child_count++] = params; }
    node->ast_children[node->ast_child_count++] = body;
    return node;
}

static AstNode* parse_top_level(Parser* p) {
    if (check(p, TOK_KW_CLASS)) {
        return parse_class_decl(p);
    }

    if (check(p, TOK_KW_STRUCT)) {
        return parse_struct_decl(p);
    }

    if (check(p, TOK_KW_INTERFACE)) {
        return parse_interface_decl(p);
    }

    if (is_primitive_type_token(p) ||
        (check(p, TOK_IDENT) && is_type_name(p->current.text)) ||
        (check(p, TOK_IDENT) && strcmp(p->current.text, "void") == 0)) {
        Type ret_type = parse_type(p);
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "error at %d:%d: expected function name\n",
                    p->current.line, p->current.col);
            p->had_error = 1;
            advance(p);
            return NULL;
        }
        return parse_func_decl(p, ret_type);
    }

    fprintf(stderr, "error at %d:%d: unexpected token '%s' at top level\n",
            p->current.line, p->current.col, p->current.text);
    p->had_error = 1;
    advance(p);
    return NULL;
}


void parser_init(Parser* p, Lexer* lexer) {
    p->lexer     = lexer;
    p->had_error = 0;
    advance(p);
    advance(p);
    advance(p);
}

AstNode* parser_parse_program(Parser* p) {
    Type void_type;
    memset(&void_type, 0, sizeof(void_type));
    void_type.type_kind = TYPE_VOID;

    AstNode* program = ast_new_node(AST_PROGRAM, p->current);
    program->ast_resolved_type = void_type;

    symtab_init();

    AstNode* decls = NULL;
    while (!check(p, TOK_EOF)) {
        AstNode* d = parse_top_level(p);
        if (d) decls = ast_append_list(decls, d);
    }

    program->ast_children[0] = decls;
    program->ast_child_count = 1;
    return program;
}

int parser_had_error(Parser* p) { return p->had_error; }
