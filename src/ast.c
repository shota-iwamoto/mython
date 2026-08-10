#include "ast.h"

#include <stdio.h>

Node *new_node(NodeKind kind, Token *tok) {
    Node *n = xmalloc(sizeof(Node));  // calloc なので他のフィールドは 0 / NULL
    n->kind = kind;
    n->tok = tok;
    return n;
}

Node *new_int_node(Token *tok, long long value) {
    Node *n = new_node(ND_INT, tok);
    n->ival = value;
    return n;
}

Node *new_binop_node(Token *tok, OpKind op, Node *lhs, Node *rhs) {
    Node *n = new_node(ND_BINOP, tok);
    n->op = op;
    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

Node *new_var_node(Token *tok, char *name) {
    Node *n = new_node(ND_VAR, tok);
    n->name = name;
    return n;
}

Node *new_unary_node(Token *tok, OpKind op, Node *operand) {
    Node *n = new_node(ND_UNARY, tok);
    n->op = op;
    n->lhs = operand;  // 単項演算は lhs だけを使う
    return n;
}

const char *op_symbol(OpKind op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_TRUEDIV: return "/";
        case OP_FLOORDIV: return "//";
        case OP_MOD: return "%";
        case OP_BITAND: return "&";
        case OP_BITOR: return "|";
        case OP_BITXOR: return "^";
        case OP_SHL: return "<<";
        case OP_SHR: return ">>";
        case OP_NEG: return "-";
        case OP_POS: return "+";
        case OP_BITNOT: return "~";
        default: UNREACHABLE();
    }
}

// ── S 式ダンプ ─────────────────────────────────────────────
// 第2章以降、ノード種別が増えるたびにここに case を足します。
// インデント付きで出力するので、深い木でも構造が読めます。

static void dump(Node *n, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");

    if (!n) {
        printf("(nil)\n");
        return;
    }

    switch (n->kind) {
        case ND_INT:
            printf("(int %lld)\n", n->ival);
            break;
        case ND_BINOP:
            printf("(binop %s\n", op_symbol(n->op));
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_UNARY:
            printf("(unary %s\n", op_symbol(n->op));
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_VAR:
            printf("(var %s)\n", n->name);
            break;
        case ND_VARDECL:
            printf("(vardecl %s %s\n", n->name, n->type_name);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_ASSIGN:
            printf("(assign\n");
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_BLOCK:
            printf("(block\n");
            for (Node *s = n->body; s; s = s->next) dump(s, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        default:
            UNREACHABLE();
    }
}

void dump_ast(Node *node) { dump(node, 0); }
