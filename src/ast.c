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

Node *new_bool_node(Token *tok, bool value) {
    Node *n = new_node(ND_BOOL, tok);
    n->ival = value ? 1 : 0;
    return n;
}

Node *new_str_node(Token *tok, char *bytes, int len) {
    Node *n = new_node(ND_STR, tok);
    n->sval = bytes;
    n->slen = len;
    return n;
}

Node *new_binop_node(Token *tok, OpKind op, Node *lhs, Node *rhs) {
    Node *n = new_node(ND_BINOP, tok);
    n->op = op;
    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

// and / or 専用。形は二項演算と同じだが、コード生成が違うので種別を分ける。
Node *new_logical_node(Token *tok, OpKind op, Node *lhs, Node *rhs) {
    Node *n = new_node(ND_LOGICAL, tok);
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
        case OP_POW: return "**";
        case OP_EQ: return "==";
        case OP_NE: return "!=";
        case OP_LT: return "<";
        case OP_LE: return "<=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        case OP_AND: return "and";
        case OP_OR: return "or";
        case OP_IS: return "is";        // 第15章
        case OP_ISNOT: return "is not";
        case OP_NEG: return "-";
        case OP_POS: return "+";
        case OP_BITNOT: return "~";
        case OP_NOT: return "not";
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
        case ND_BOOL:
            printf("(bool %s)\n", n->ival ? "True" : "False");
            break;
        case ND_STR:
            printf("(str %d bytes)\n", n->slen);
            break;
        case ND_LOGICAL:
            printf("(logical %s\n", op_symbol(n->op));
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
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
        case ND_NONE:  // 第15章
            printf("None");
            return;

        case ND_VAR:
            printf("(var %s)\n", n->name);
            break;
        case ND_TYPEREF:
            if (!n->lhs) { printf("(type %s)\n", n->name); break; }
            printf("(type %s\n", n->name);
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_LIST:
            printf("(list\n");
            for (Node *el = n->body; el; el = el->next) dump(el, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_INDEX:
            printf("(index\n");
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_METHOD:
            printf("(method %s\n", n->name);
            dump(n->lhs, depth + 1);
            for (Node *a = n->args; a; a = a->next) dump(a, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_FIELD:
            printf("(field %s\n", n->name);
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_CLASS:
            printf("(class %s\n", n->name);
            for (Node *m = n->body; m; m = m->next) dump(m, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_FIELDDECL:
            printf("(fielddecl %s\n", n->name);
            dump(n->type_ref, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_VARDECL:
            printf("(vardecl %s\n", n->name);
            dump(n->type_ref, depth + 1);
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
        case ND_IF:
            printf("(if\n");
            dump(n->lhs, depth + 1);
            dump(n->body, depth + 1);
            if (n->els) dump(n->els, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_WHILE:
            printf("(while\n");
            dump(n->lhs, depth + 1);
            dump(n->body, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_BREAK:
            printf("(break)\n");
            break;
        case ND_CONTINUE:
            printf("(continue)\n");
            break;
        case ND_PASS:
            printf("(pass)\n");
            break;
        case ND_PRINT:
            printf("(print\n");
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_FUNC:
            printf("(func %s\n", n->name);
            dump(n->type_ref, depth + 1);
            for (Node *pm = n->params; pm; pm = pm->next) dump(pm, depth + 1);
            dump(n->body, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_PARAM:
            printf("(param %s\n", n->name);
            dump(n->type_ref, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_CALL:
            printf("(call %s\n", n->name);
            for (Node *a = n->args; a; a = a->next) dump(a, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_RETURN:
            if (!n->lhs) { printf("(return)\n"); break; }
            printf("(return\n");
            dump(n->lhs, depth + 1);
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
