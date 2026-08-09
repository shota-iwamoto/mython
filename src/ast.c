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
        default:
            UNREACHABLE();
    }
}

void dump_ast(Node *node) { dump(node, 0); }
