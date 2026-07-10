#include "parser.h"
#include "symtab.h"
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

static int is_type_name(const char* name) {
    if (strcmp(name, "int") == 0 || strcmp(name, "char") == 0) return 1;
    return symtab_find_class(name) != NULL;
}

/* ================================================================
   FORWARD DECLARATIONS
   ================================================================ */

static AstNode* parse_stmt(Parser* p);
static AstNode* parse_expr(Parser* p);

/* �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??   TYPE PARSING
   �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??*/

static Type parse_type(Parser* p) {
    Type t;
    memset(&t, 0, sizeof(t));

    if (check(p, TOK_KW_INT)) {
        advance(p);
        t.kind = TYPE_INT;
    } else if (check(p, TOK_KW_CHAR)) {
        advance(p);
        t.kind = TYPE_CHAR;
    } else if (check(p, TOK_IDENT) && is_type_name(p->current.text)) {
        t.kind = TYPE_CLASS;
        strncpy(t.class_name, p->current.text, 63);
        t.class_name[63] = '\0';
        t.is_pointer = 1;
        advance(p);
    } else {
        t.kind = TYPE_VOID;
    }

    if (check(p, TOK_LBRACKET)) {
        advance(p);
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
    }
    return t;
}

/* �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??   EXPRESSION PARSING
   �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??*/

static AstNode* parse_primary(Parser* p) {
    if (check(p, TOK_INT_LIT)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_INT_LIT, t);
        n->resolved_type.kind = TYPE_INT;
        return n;
    }
    if (check(p, TOK_CHAR_LIT)) {
        Token t = p->current; advance(p);
        AstNode* n = ast_new_node(AST_CHAR_LIT, t);
        n->resolved_type.kind = TYPE_CHAR;
        return n;
    }
    if (check(p, TOK_IDENT)) {
        Token t = p->current; advance(p);
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

        Type base;
        memset(&base, 0, sizeof(base));

        if (check(p, TOK_KW_INT))       { advance(p); base.kind = TYPE_INT; }
        else if (check(p, TOK_KW_CHAR)) { advance(p); base.kind = TYPE_CHAR; }
        else if (check(p, TOK_IDENT))   {
            base.kind = TYPE_CLASS;
            strncpy(base.class_name, p->current.text, 63);
            base.class_name[63] = '\0';
            advance(p);
        } else {
            fprintf(stderr, "error at %d:%d: expected type after 'new'\n",
                    p->current.line, p->current.col);
            p->had_error = 1;
            return NULL;
        }

        AstNode* node = ast_new_node(AST_NEW, new_tok);
        node->resolved_type = base;

        if (check(p, TOK_LBRACKET)) {
            advance(p);
            node->children[0] = parse_expr(p);
            node->child_count = 1;
            expect(p, TOK_RBRACKET);
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
            AstNode* idx = parse_expr(p);
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
                args = ast_append_list(args, parse_expr(p));
                while (check(p, TOK_COMMA)) {
                    advance(p);
                    args = ast_append_list(args, parse_expr(p));
                }
                call->children[1] = args;
                call->child_count = 2;
            }
            expect(p, TOK_RPAREN);
            node = call;
        } else {
            break;
        }
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

/* �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??   STATEMENT PARSING
   �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??*/

static AstNode* parse_block(Parser* p) {
    Token brace = p->current; advance(p); /* consume { */
    AstNode* block = ast_new_node(AST_BLOCK, brace);
    AstNode* stmts = NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        AstNode* s = parse_stmt(p);
        if (s) stmts = ast_append_list(stmts, s);
    }
    expect(p, TOK_RBRACE);
    block->children[0] = stmts;
    block->child_count = 1;
    return block;
}

static int stmt_looks_like_var_decl(Parser* p) {
    if (check(p, TOK_KW_INT) || check(p, TOK_KW_CHAR)) return 1;
    if (check(p, TOK_IDENT) && is_type_name(p->current.text)) {
        TokenKind next = p->peek.kind;
        if (next == TOK_IDENT) return 1;
        if (next == TOK_LBRACKET) return 1;
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
        if (init && init->kind == AST_NEW && type.kind == TYPE_CLASS) {
            type.is_pointer = 1;
        }
    }

    node->resolved_type = type;
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
            ast_add_child(node, parse_expr(p));
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
        if (expr) {
            AstNode* es = ast_new_node(AST_EXPR_STMT, expr->tok);
            ast_add_child(es, expr);
            expect(p, TOK_SEMI);
            return es;
        }
    }

    return NULL;
}

/* �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??   TOP-LEVEL PARSING
   �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??*/

static AstNode* parse_class_decl(Parser* p) {
    Token kw = p->current; advance(p); /* struct */

    if (!check(p, TOK_IDENT)) {
        fprintf(stderr, "error at %d:%d: expected struct name\n",
                p->current.line, p->current.col);
        p->had_error = 1;
        return NULL;
    }
    Token name = p->current; advance(p);
    expect(p, TOK_LBRACE);

    ClassInfo* info = calloc(1, sizeof(ClassInfo));
    strncpy(info->name, name.text, 63);
    info->name[63] = '\0';

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Type ft = parse_type(p);
        if (!check(p, TOK_IDENT)) {
            fprintf(stderr, "error at %d:%d: expected field name\n",
                    p->current.line, p->current.col);
            p->had_error = 1;
            break;
        }
        Token fname = p->current; advance(p);
        expect(p, TOK_SEMI);
        symtab_add_field(info, fname.text, ft);
    }
    expect(p, TOK_RBRACE);

    symtab_add_class(name.text, info);

    AstNode* node = ast_new_node(AST_CLASS_DECL, name);
    node->resolved_type.kind = TYPE_CLASS;
    strncpy(node->resolved_type.class_name, name.text, 63);
    return node;
}

static AstNode* parse_func_decl(Parser* p, Type ret_type) {
    Token name = p->current; advance(p);
    expect(p, TOK_LPAREN);

    symtab_enter_scope();

    AstNode* params = NULL;
    if (!check(p, TOK_RPAREN)) {
        do {
            Type pt = parse_type(p);
            if (!check(p, TOK_IDENT)) {
                fprintf(stderr, "error at %d:%d: expected parameter name\n",
                        p->current.line, p->current.col);
                p->had_error = 1;
                break;
            }
            Token pname = p->current; advance(p);
            AstNode* pd = ast_new_node(AST_VAR_DECL, pname);
            pd->resolved_type = pt;
            symtab_insert(pname.text, pt);
            params = ast_append_list(params, pd);
        } while (check(p, TOK_COMMA) && (advance(p), 1));
    }
    expect(p, TOK_RPAREN);

    AstNode* body = parse_stmt(p);
    symtab_exit_scope();

    symtab_add_func(name.text, ret_type);

    AstNode* node = ast_new_node(AST_FUNC_DECL, name);
    node->resolved_type = ret_type;
    if (params) { node->children[node->child_count++] = params; }
    node->children[node->child_count++] = body;
    return node;
}

static AstNode* parse_top_level(Parser* p) {
    if (check(p, TOK_KW_CLASS)) {
        return parse_class_decl(p);
    }

    if (check(p, TOK_KW_INT) || check(p, TOK_KW_CHAR) ||
        (check(p, TOK_IDENT) && is_type_name(p->current.text))) {
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

/* �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??   PUBLIC INTERFACE
   �T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T�T??*/

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
    void_type.kind = TYPE_VOID;

    AstNode* program = ast_new_node(AST_PROGRAM, p->current);
    program->resolved_type = void_type;

    symtab_init();

    AstNode* decls = NULL;
    while (!check(p, TOK_EOF)) {
        AstNode* d = parse_top_level(p);
        if (d) decls = ast_append_list(decls, d);
    }

    program->children[0] = decls;
    program->child_count = 1;
    return program;
}

int parser_had_error(Parser* p) { return p->had_error; }
